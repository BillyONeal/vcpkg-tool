import * as cache from '@actions/cache';
import { text } from 'stream/consumers';

const operations = [];
const jsonData = JSON.parse(await text(process.stdin));

if (Array.isArray(jsonData.push)) {
    for (const operation of jsonData.push) {
        const directory = operation.directory;
        const key = operation.key;
        operations.push(cache.saveCache([directory], key));
    }
}

const restores = [];
if (Array.isArray(jsonData.restore)) {
    for (const operation of jsonData.restore) {
        const directory = operation.directory;
        const key = operation.key;
        operations.push(cache.restoreCache([directory], key)
            .then((cacheKeyOrUndefined) => {
                if (cacheKeyOrUndefined) {
                    restores.push(key);
                }
            }));
    }
}

await Promise.all(operations);
console.log(JSON.stringify(restores));
