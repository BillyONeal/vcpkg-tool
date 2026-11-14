. $PSScriptRoot/../end-to-end-tests-prelude.ps1

$portName = 'vcpkg-spdx-test'
$overlay = Join-Path $TestingRoot 'spdx-port'
New-Item -ItemType Directory -Path $overlay | Out-Null
Set-Content -LiteralPath (Join-Path $overlay 'vcpkg.json') -Value @'
{
  "name": "vcpkg-spdx-test",
  "version": "1.0",
  "port-version": 5,
  "license": "MIT OR Apache-2.0",
  "features": {
    "unknown": {
      "description": "A feature with unknown licensing",
      "license": null
    }
  }
}
'@
Set-Content -LiteralPath (Join-Path $overlay 'portfile.cmake') -Value @'
set(VCPKG_POLICY_EMPTY_PACKAGE enabled)
set(share "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(MAKE_DIRECTORY "${share}")
file(WRITE "${share}/copyright" "SPDX test fixture\n")
file(WRITE "${share}/payload.txt" "installed contents\n")
file(WRITE "${share}/vcpkg.spdx.json" "stale SPDX 2 document")
file(WRITE "${share}/vcpkg.spdx3.json" "stale SPDX 3 document")
string(REPEAT "a" 128 sha512)
file(WRITE "${share}/vcpkg-spdx-resources.json" "{
  \"packages\": [{
    \"SPDXID\": \"SPDXRef-resource-0\",
    \"name\": \"source archive\",
    \"downloadLocation\": \"https://example.org/source.tar.gz\",
    \"packageFileName\": \"source.tar.gz\",
    \"checksums\": [{\"algorithm\": \"SHA512\", \"checksumValue\": \"${sha512}\"}],
    \"licenseConcluded\": \"NOASSERTION\",
    \"licenseDeclared\": \"NOASSERTION\",
    \"copyrightText\": \"NOASSERTION\",
    \"extra\": {\"nested\": [\"preserved\", 42]}
  }]
}")
'@

$commonArgs += @("--overlay-ports=$overlay", "--host-triplet=$Triplet")
Run-Vcpkg @commonArgs install "$portName[unknown]" "--x-binarysource=clear;files,$ArchiveRoot,write" --clean-packages-after-build
Throw-IfFailed

$share = Join-Path $installRoot "$Triplet/share/$portName"
$spdx2Path = Join-Path $share 'vcpkg.spdx.json'
$spdx3Path = Join-Path $share 'vcpkg.spdx3.json'
Require-FileExists $spdx2Path
Require-FileExists $spdx3Path
$spdx2 = Get-Content -LiteralPath $spdx2Path -Raw | ConvertFrom-Json
$spdx3Text = Get-Content -LiteralPath $spdx3Path -Raw
$spdx3 = $spdx3Text | ConvertFrom-Json
Throw-IfNonEqual -Actual $spdx2.spdxVersion -Expected 'SPDX-2.3'
Throw-IfNonEqual -Actual $spdx3.'@context' -Expected 'https://spdx.org/rdf/3.0.1/spdx-context.jsonld'
$graph = $spdx3.'@graph'
$creation = $graph | Where-Object type -eq 'CreationInfo'
Throw-IfNonEqual -Actual $creation.specVersion -Expected '3.0.1'
Throw-IfNonEqual -Actual $creation.created -Expected $spdx2.creationInfo.created

foreach ($file in $spdx2.files) {
    if ($file.fileName -match '/vcpkg\.spdx3?\.json$') {
        throw 'Generated SBOMs must not hash themselves or each other'
    }
    $translated = @($graph | Where-Object spdxId -Like "*#$($file.SPDXID)")
    Throw-IfNonEqual -Actual $translated.Count -Expected 1
    Throw-IfNonEqual -Actual $translated[0].name -Expected $file.fileName
    Throw-IfNonEqual -Actual $translated[0].verifiedUsing[0].hashValue -Expected $file.checksums[0].checksumValue
    Throw-IfNonEqual -Actual $translated[0].verifiedUsing[0].algorithm -Expected 'sha256'
}

$resource = $graph | Where-Object spdxId -Like '*#SPDXRef-resource-0'
Throw-IfNonEqual -Actual $resource.software_downloadLocation -Expected 'https://example.org/source.tar.gz'
$distribution = $graph | Where-Object spdxId -Like '*#SPDXRef-resource-0-distribution'
Throw-IfNonEqual -Actual $distribution.name -Expected 'source.tar.gz'
Throw-IfNonEqual -Actual $distribution.verifiedUsing[0].algorithm -Expected 'sha512'
Throw-IfNonEqual -Actual $distribution.verifiedUsing[0].hashValue -Expected ('a' * 128)
$annotations = @($graph | Where-Object { $_.type -eq 'Annotation' -and $_.subject -eq $resource.spdxId })
Throw-IfNonEqual -Actual $annotations.Count -Expected 1
$preserved = $annotations[0].statement | ConvertFrom-Json
Throw-IfNonEqual -Actual $preserved.extra.nested[0] -Expected 'preserved'
Throw-IfNonEqual -Actual $preserved.extra.nested[1] -Expected 42

Run-Vcpkg @commonArgs remove $portName
Throw-IfFailed
Require-FileNotExists $spdx2Path
Require-FileNotExists $spdx3Path
Run-Vcpkg @commonArgs install "$portName[unknown]" "--x-binarysource=clear;files,$ArchiveRoot,read" --only-binarycaching
Throw-IfFailed
Require-FileExists $spdx2Path
Require-FileExists $spdx3Path
Throw-IfNonEqual -Actual (Get-Content -LiteralPath $spdx3Path -Raw) -Expected $spdx3Text

Run-Vcpkg @commonArgs export $portName --raw --output=spdx-export "--output-dir=$TestingRoot"
Throw-IfFailed
$exportedShare = Join-Path $TestingRoot "spdx-export/installed/$Triplet/share/$portName"
Require-FileExists (Join-Path $exportedShare 'vcpkg.spdx.json')
Require-FileExists (Join-Path $exportedShare 'vcpkg.spdx3.json')
Throw-IfNonEqual -Actual (Get-Content -LiteralPath (Join-Path $exportedShare 'vcpkg.spdx3.json') -Raw) -Expected $spdx3Text
