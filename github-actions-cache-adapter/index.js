const cache = require('@actions/cache');

// Parse command line arguments for a file to write to
const args = process.argv.slice(2);
if (args.length < 1) {
    console.error('Usage: node index.js <mode> <cache-key> <target-directory>');
    process.exit(1);
}

const mode = args[0];
if (mode === 'push-success') {
    const key = args[1];
    const directory = args[2];
    await cache.saveCache([directory], key);
} else if (mode === 'restore') {
    const key = args[1];
    const directory = args[2];
    await cache.restoreCache([directory], key);
} else {
    console.error('Invalid mode. Use "push-success" or "restore".');
    process.exit(1);
}
