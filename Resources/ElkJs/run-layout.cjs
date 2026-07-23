/*
 * Copyright (c) GraphFormatter contributors.
 * Licensed under the MIT License. See LICENSE in the GraphFormatter plugin root.
 */

'use strict';

const fs = require('fs');
const path = require('path');
const ELK = require(path.join(__dirname, 'package', 'lib', 'elk.bundled.js'));

async function main() {
    const [, , inputFilename, outputFilename] = process.argv;
    if (!inputFilename || !outputFilename) {
        throw new Error('Usage: node run-layout.cjs <input.json> <output.json>');
    }

    const graph = JSON.parse(fs.readFileSync(inputFilename, 'utf8'));
    const elk = new ELK();
    const laidOutGraph = await elk.layout(graph);
    fs.writeFileSync(outputFilename, JSON.stringify(laidOutGraph, null, 2), 'utf8');
}

main().catch(error => {
    process.stderr.write(`${error && error.stack ? error.stack : String(error)}\n`);
    process.exitCode = 1;
});
