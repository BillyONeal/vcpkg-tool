. $PSScriptRoot/../end-to-end-tests-prelude.ps1

$NativeSlash = '/'
if ($IsWindows) {
	$NativeSlash = '\'
}

Refresh-TestRoot
[string]$buildOutput = Run-VcpkgAndCaptureStderr install @commonArgs --overlay-ports="$PSScriptRoot/../e2e-ports" vcpkg-policy-set-incorrectly
Throw-IfNotFailed
if (-not $buildOutput.Replace("`r`n", "`n").EndsWith("error: Unknown setting of VCPKG_POLICY_EMPTY_PACKAGE: ON. Valid policy values are '', 'disabled', and 'enabled'.`n")) {
    throw ('Incorrect error message for incorrect policy value; output was ' + $buildOutput)
}

$buildOutput = Run-VcpkgAndCaptureOutput install @commonArgs --overlay-ports="$PSScriptRoot/../e2e-ports" vcpkg-policy-empty-package --no-binarycaching
Throw-IfFailed
if (-not $buildOutput.Contains('Skipping post-build validation due to VCPKG_POLICY_EMPTY_PACKAGE')) {
    throw ('Didn''t skip post-build checks correctly, output was ' + $buildOutput)
}

$buildOutput = Run-VcpkgAndCaptureOutput install @commonArgs --overlay-ports="$PSScriptRoot/../e2e-ports" vcpkg-policy-skip-all-post-build-checks --no-binarycaching
Throw-IfFailed
if (-not $buildOutput.Contains('Skipping post-build validation due to VCPKG_POLICY_SKIP_ALL_POST_BUILD_CHECKS')) {
    throw ('Didn''t skip post-build checks correctly, output was ' + $buildOutput)
}

Refresh-TestRoot
$VcpkgIncludeFolderPoliciesPath = "$PSScriptRoot/../e2e-ports$($NativeSlash)vcpkg-include-folder-policies$($NativeSlash)portfile.cmake"
$buildOutput = Run-VcpkgAndCaptureOutput @commonArgs install --overlay-ports="$PSScriptRoot/../e2e-ports" vcpkg-include-folder-policies --no-binarycaching --enforce-port-checks
Throw-IfNotFailed
[string]$expected = @"
$($VcpkgIncludeFolderPoliciesPath): warning: The folder $`{CURRENT_PACKAGES_DIR}/include is empty or not present. This usually means that headers are not correctly installed. If this is a CMake helper port, add set(VCPKG_POLICY_CMAKE_HELPER_PORT enabled). If this is not a CMake helper port but this is otherwise intentional, add set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled) to suppress this message.
"@
if (-not $buildOutput.Contains($expected)) {
    throw 'Did not detect empty include folder'
}

$buildOutput = Run-VcpkgAndCaptureOutput @commonArgs install --overlay-ports="$PSScriptRoot/../e2e-ports" 'vcpkg-include-folder-policies[policy-empty-include-folder]' --no-binarycaching --enforce-port-checks
Throw-IfFailed
if ($buildOutput.Contains($expected)) {
    throw 'VCPKG_POLICY_EMPTY_INCLUDE_FOLDER didn''t supress'
}

Refresh-TestRoot
$buildOutput = Run-VcpkgAndCaptureOutput @commonArgs install --overlay-ports="$PSScriptRoot/../e2e-ports" 'vcpkg-include-folder-policies[do-install,policy-cmake-helper-port]' --no-binarycaching --enforce-port-checks
Throw-IfNotFailed
$expected = @"
$($VcpkgIncludeFolderPoliciesPath): warning: The folder $`{CURRENT_PACKAGES_DIR}/include exists in a CMake helper port; this is incorrect, since only CMake files should be installed. To suppress this message, remove set(VCPKG_POLICY_CMAKE_HELPER_PORT enabled).
"@
if (-not $buildOutput.Contains($expected)) {
    throw 'Did not detect nonempty include folder for CMake helper port.'
}

Refresh-TestRoot
$buildOutput = Run-VcpkgAndCaptureOutput @commonArgs install --overlay-ports="$PSScriptRoot/../e2e-ports" 'vcpkg-include-folder-policies[policy-cmake-helper-port]' --no-binarycaching --enforce-port-checks
Throw-IfNotFailed
$expected = @"
$($VcpkgIncludeFolderPoliciesPath): warning: The $`{CURRENT_PACKAGES_DIR}/share/vcpkg-include-folder-policies/vcpkg-port-config.cmake file does not exist. This file must exist for CMake helper ports. To suppress this message, remove set(VCPKG_POLICY_CMAKE_HELPER_PORT enabled) from portfile.cmake.
"@
if (-not $buildOutput.Contains($expected)) {
    throw 'Did not detect missing vcpkg-port-config.cmake for CMake helper port.'
}

$buildOutput = Run-VcpkgAndCaptureOutput @commonArgs install --overlay-ports="$PSScriptRoot/../e2e-ports" 'vcpkg-include-folder-policies[policy-cmake-helper-port,do-install-vcpkg-port-config]' --no-binarycaching --enforce-port-checks
Throw-IfFailed

Refresh-TestRoot
$buildOutput = Run-VcpkgAndCaptureOutput @commonArgs install --overlay-ports="$PSScriptRoot/../e2e-ports" 'vcpkg-include-folder-policies[do-install-restricted]' --no-binarycaching --enforce-port-checks
Throw-IfNotFailed
$expected = @"
warning: The following restricted headers can prevent the core C++ runtime and other packages from compiling correctly. In exceptional circumstances, this policy can be disabled by adding set(VCPKG_POLICY_ALLOW_RESTRICTED_HEADERS enabled) to portfile.cmake.

  $`{CURRENT_PACKAGES_DIR}/include/json.h
"@

if (-not $buildOutput.Replace("`r`n", "`n").Contains($expected)) {
    throw 'Did not detect restricted header'
}

$buildOutput = Run-VcpkgAndCaptureOutput @commonArgs install --overlay-ports="$PSScriptRoot/../e2e-ports" 'vcpkg-include-folder-policies[do-install-restricted,policy-allow-restricted-headers]' --no-binarycaching --enforce-port-checks
Throw-IfFailed
if ($buildOutput.Replace("`r`n", "`n").Contains($expected)) {
    throw 'VCPKG_POLICY_ALLOW_RESTRICTED_HEADERS didn''t allow'
}







##################################


if (-not $IsWindows) {
    Write-Host 'Skipping Windows-specific e2e post build checks on non-Windows'
    return
}

# DLLs with no exports
Refresh-TestRoot
$buildOutput = Run-VcpkgAndCaptureOutput @commonArgs install --overlay-ports="$PSScriptRoot/../e2e-ports" vcpkg-internal-dll-with-no-exports --no-binarycaching --enforce-port-checks
Throw-IfNotFailed
if (-not $buildOutput.Contains("$packagesRoot\vcpkg-internal-dll-with-no-exports_x86-windows\debug\bin\no_exports.dll") `
    -or -not $buildOutput.Contains("$packagesRoot\vcpkg-internal-dll-with-no-exports_x86-windows\bin\no_exports.dll") `
    -or -not $buildOutput.Contains('set(VCPKG_POLICY_DLLS_WITHOUT_EXPORTS enabled)')) {
    throw 'Did not detect DLLs with no exports.'
}

Refresh-TestRoot
$buildOutput = Run-VcpkgAndCaptureOutput @commonArgs install --overlay-ports="$PSScriptRoot/../e2e-ports" vcpkg-internal-dll-with-no-exports[policy] --no-binarycaching --enforce-port-checks
Throw-IfFailed
if ($buildOutput.Contains("$packagesRoot\vcpkg-internal-dll-with-no-exports_x86-windows\debug\bin\no_exports.dll") `
    -or $buildOutput.Contains("$packagesRoot\vcpkg-internal-dll-with-no-exports_x86-windows\bin\no_exports.dll") `
    -or $buildOutput.Contains('set(VCPKG_POLICY_DLLS_WITHOUT_EXPORTS enabled)')) {
    throw 'VCPKG_POLICY_DLLS_WITHOUT_EXPORTS didn''t suppress'
}

# DLLs with wrong architecture
Refresh-TestRoot
mkdir "$TestingRoot/wrong-architecture"
Copy-Item -Recurse "$PSScriptRoot/../e2e-assets/test-dll-port-template" "$TestingRoot/wrong-architecture/test-dll"
Run-Vcpkg env "$TestingRoot/wrong-architecture/test-dll/build.cmd" --Triplet x64-windows

$buildOutput = Run-VcpkgAndCaptureOutput @commonArgs install --overlay-ports="$TestingRoot/wrong-architecture" test-dll --no-binarycaching --enforce-port-checks
Throw-IfNotFailed
$expected = "warning: The following files were built for an incorrect architecture. To suppress this message, add set(VCPKG_POLICY_SKIP_ARCHITECTURE_CHECK enabled) to portfile.cmake.`n" + `
"warning:   $packagesRoot\test-dll_x86-windows\debug\lib\test_dll.lib`n" + `
" Expected: x86, but was x64`n" + `
"warning: The following files were built for an incorrect architecture:`n" + `
"warning:   $packagesRoot\test-dll_x86-windows\lib\test_dll.lib`n" + `
" Expected: x86, but was x64`n" + `
"warning: The following files were built for an incorrect architecture. To suppress this message, add set(VCPKG_POLICY_SKIP_ARCHITECTURE_CHECK enabled) to portfile.cmake.`n" + `
"warning:   $packagesRoot\test-dll_x86-windows\debug\bin\test_dll.dll`n" + `
" Expected: x86, but was x64`n" + `
"warning:   $packagesRoot\test-dll_x86-windows\bin\test_dll.dll`n" + `
" Expected: x86, but was x64`n"

if (-not $buildOutput.Replace("`r`n", "`n").Contains($expected)) {
    throw 'Did not detect DLL with wrong architecture.'
}

$buildOutput = Run-VcpkgAndCaptureOutput @commonArgs install --overlay-ports="$TestingRoot/wrong-architecture" test-dll[policy-skip-architecture-check] --no-binarycaching --enforce-port-checks
Throw-IfFailed
if ($buildOutput.Contains("warning: The following files were built for an incorrect architecture. To suppress this message, add set(VCPKG_POLICY_SKIP_ARCHITECTURE_CHECK enabled) to portfile.cmake.")) {
    throw 'VCPKG_POLICY_SKIP_ARCHITECTURE_CHECK didn''t suppress'
}

# DLLs with no AppContainer bit
Refresh-TestRoot
mkdir "$TestingRoot/wrong-appcontainer"
Copy-Item -Recurse "$PSScriptRoot/../e2e-assets/test-dll-port-template" "$TestingRoot/wrong-appcontainer/test-dll"
Run-Vcpkg env "$TestingRoot/wrong-appcontainer/test-dll/build.cmd" --Triplet x64-windows

$buildOutput = Run-VcpkgAndCaptureOutput --triplet x64-uwp "--x-buildtrees-root=$buildtreesRoot" "--x-install-root=$installRoot" "--x-packages-root=$packagesRoot" install --overlay-ports="$TestingRoot/wrong-appcontainer" test-dll --no-binarycaching --enforce-port-checks
Throw-IfNotFailed
$expected = "warning: The App Container bit must be set for all DLLs in Windows Store apps, but the following DLLs were not built with it turned on. To suppress this message, add set(VCPKG_POLICY_SKIP_APPCONTAINER_CHECK enabled) to portfile.cmake.`n" + `
"`n" + `
"  $packagesRoot\test-dll_x64-uwp\debug\bin\test_dll.dll`n" + `
"  $packagesRoot\test-dll_x64-uwp\bin\test_dll.dll`n"

if (-not $buildOutput.Replace("`r`n", "`n").Contains($expected)) {
    throw 'Did not detect DLL with wrong appcontainer.'
}

$buildOutput = Run-VcpkgAndCaptureOutput --triplet x64-uwp "--x-buildtrees-root=$buildtreesRoot" "--x-install-root=$installRoot" "--x-packages-root=$packagesRoot" install --overlay-ports="$TestingRoot/wrong-appcontainer" test-dll[policy-skip-appcontainer-check] --no-binarycaching --enforce-port-checks
Throw-IfFailed
if ($buildOutput.Contains("warning: The App Container bit must be set")) {
    throw 'VCPKG_POLICY_SKIP_APPCONTAINER_CHECK didn''t suppress'
}

# Wrong CRT linkage
Refresh-TestRoot
mkdir "$TestingRoot/wrong-crt"
Copy-Item -Recurse "$PSScriptRoot/../e2e-assets/test-lib-port-template-dynamic-crt" "$TestingRoot/wrong-crt/test-lib"
Run-Vcpkg env "$TestingRoot/wrong-crt/test-lib/build.cmd" --Triplet x86-windows-static

$buildOutput = Run-VcpkgAndCaptureOutput --triplet x86-windows-static "--x-buildtrees-root=$buildtreesRoot" "--x-install-root=$installRoot" "--x-packages-root=$packagesRoot" install --overlay-ports="$TestingRoot/wrong-crt" test-lib --no-binarycaching --enforce-port-checks
Throw-IfNotFailed
$expected = "warning: The following binaries should use the Static Debug (/MTd) CRT.`n" +
"  $packagesRoot\test-lib_x86-windows-static\debug\lib\both_lib.lib links with:`n" +
"    Dynamic Debug (/MDd)`n" +
"    Dynamic Release (/MD)`n" +
"  $packagesRoot\test-lib_x86-windows-static\debug\lib\test_lib.lib links with: Dynamic Debug (/MDd)`n" +
"To inspect the lib files, use:`n" +
"  dumpbin.exe /directives mylibfile.lib`n" +
"If the triplet is intended to only use the release CRT, you should add set(VCPKG_POLICY_ONLY_RELEASE_CRT enabled) to the triplet .cmake file. To suppress this check entirely, add set(VCPKG_POLICY_SKIP_CRT_LINKAGE_CHECK enabled) to the triplet .cmake if this is triplet-wide, or to portfile.cmake if this is specific to a port.`n" +
"warning: The following binaries should use the Static Release (/MT) CRT.`n" +
"  $packagesRoot\test-lib_x86-windows-static\lib\both_lib.lib links with:`n" +
"    Dynamic Debug (/MDd)`n" +
"    Dynamic Release (/MD)`n" +
"  $packagesRoot\test-lib_x86-windows-static\lib\test_lib.lib links with: Dynamic Release (/MD)`n" +
"To inspect the lib files, use:`n" +
"  dumpbin.exe /directives mylibfile.lib`n" +
"If the triplet is intended to only use the release CRT, you should add set(VCPKG_POLICY_ONLY_RELEASE_CRT enabled) to the triplet .cmake file. To suppress this check entirely, add set(VCPKG_POLICY_SKIP_CRT_LINKAGE_CHECK enabled) to the triplet .cmake if this is triplet-wide, or to portfile.cmake if this is specific to a port.`n"

if (-not $buildOutput.Replace("`r`n", "`n").Contains($expected)) {
    throw 'Did not detect lib with wrong CRT linkage.'
}

$buildOutput = Run-VcpkgAndCaptureOutput --triplet x86-windows-static "--x-buildtrees-root=$buildtreesRoot" "--x-install-root=$installRoot" "--x-packages-root=$packagesRoot" install --overlay-ports="$TestingRoot/wrong-crt" test-lib[policy-skip-crt-linkage-check] --no-binarycaching --enforce-port-checks
Throw-IfFailed
if ($buildOutput.Contains('warning: The following binaries should use the Static Debug (/MTd) CRT.')) {
    throw 'VCPKG_POLICY_SKIP_CRT_LINKAGE_CHECK didn''t suppress'
}

# ... also release only
Refresh-TestRoot
mkdir "$TestingRoot/wrong-crt-release-only"
Copy-Item -Recurse "$PSScriptRoot/../e2e-assets/test-lib-port-template-dynamic-crt-release-only" "$TestingRoot/wrong-crt-release-only/test-lib"
Run-Vcpkg env "$TestingRoot/wrong-crt-release-only/test-lib/build.cmd" --Triplet x86-windows-static

$buildOutput = Run-VcpkgAndCaptureOutput --triplet x86-windows-static "--x-buildtrees-root=$buildtreesRoot" "--x-install-root=$installRoot" "--x-packages-root=$packagesRoot" install --overlay-ports="$TestingRoot/wrong-crt-release-only" test-lib --no-binarycaching --enforce-port-checks
Throw-IfNotFailed
$expected = "warning: The following binaries should use the Static Debug (/MTd) CRT.`n" +
"  $packagesRoot\test-lib_x86-windows-static\debug\lib\test_lib.lib links with: Dynamic Release (/MD)`n" +
"To inspect the lib files, use:`n" +
"  dumpbin.exe /directives mylibfile.lib`n" +
"If the triplet is intended to only use the release CRT, you should add set(VCPKG_POLICY_ONLY_RELEASE_CRT enabled) to the triplet .cmake file. To suppress this check entirely, add set(VCPKG_POLICY_SKIP_CRT_LINKAGE_CHECK enabled) to the triplet .cmake if this is triplet-wide, or to portfile.cmake if this is specific to a port.`n" +
"warning: The following binaries should use the Static Release (/MT) CRT.`n" +
"  $packagesRoot\test-lib_x86-windows-static\lib\test_lib.lib links with: Dynamic Release (/MD)`n" +
"To inspect the lib files, use:`n" +
"  dumpbin.exe /directives mylibfile.lib`n" +
"If the triplet is intended to only use the release CRT, you should add set(VCPKG_POLICY_ONLY_RELEASE_CRT enabled) to the triplet .cmake file. To suppress this check entirely, add set(VCPKG_POLICY_SKIP_CRT_LINKAGE_CHECK enabled) to the triplet .cmake if this is triplet-wide, or to portfile.cmake if this is specific to a port.`n"

if (-not $buildOutput.Replace("`r`n", "`n").Contains($expected)) {
    throw 'Did not detect lib with wrong CRT linkage release only.'
}

$buildOutput = Run-VcpkgAndCaptureOutput --triplet x86-windows-static "--x-buildtrees-root=$buildtreesRoot" "--x-install-root=$installRoot" "--x-packages-root=$packagesRoot" install --overlay-ports="$TestingRoot/wrong-crt-release-only" test-lib[policy-only-release-crt] --no-binarycaching --enforce-port-checks
Throw-IfNotFailed
$expected = "warning: The following binaries should use the Static Release (/MT) CRT.`n" +
"  $packagesRoot\test-lib_x86-windows-static\lib\test_lib.lib links with: Dynamic Release (/MD)`n" +
"To inspect the lib files, use:`n" +
"  dumpbin.exe /directives mylibfile.lib`n" +
"If the triplet is intended to only use the release CRT, you should add set(VCPKG_POLICY_ONLY_RELEASE_CRT enabled) to the triplet .cmake file. To suppress this check entirely, add set(VCPKG_POLICY_SKIP_CRT_LINKAGE_CHECK enabled) to the triplet .cmake if this is triplet-wide, or to portfile.cmake if this is specific to a port.`n"

if (-not $buildOutput.Replace("`r`n", "`n").Contains($expected)) {
    throw 'Did not detect lib with wrong CRT linkage release only.'
}

if ($buildOutput.Contains('warning: The following binaries should use the Static Debug (/MTd) CRT.')) {
    throw 'VCPKG_POLICY_ONLY_RELEASE_CRT didn''t suppress detecting debug CRTs'
}

$buildOutput = Run-VcpkgAndCaptureOutput --triplet x86-windows-static "--x-buildtrees-root=$buildtreesRoot" "--x-install-root=$installRoot" "--x-packages-root=$packagesRoot" install --overlay-ports="$TestingRoot/wrong-crt-release-only" test-lib[policy-skip-crt-linkage-check] --no-binarycaching --enforce-port-checks
Throw-IfFailed
if ($buildOutput.Contains('warning: The following binaries should use the Static Release (/MT) CRT.')) {
    throw 'VCPKG_POLICY_SKIP_CRT_LINKAGE_CHECK didn''t suppress'
}
