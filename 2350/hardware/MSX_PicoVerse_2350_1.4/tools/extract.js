// Pull `pcbdata` out of the InteractiveHtmlBom page (it is LZString-compressed).
const fs = require('fs');
const path = require('path');

const BOM = path.join(__dirname, '..', '..', 'MSX_PicoVerse_2350_1.2_bom.html');
const html = fs.readFileSync(BOM, 'utf8').split(/\r?\n/);

const lzLine = html.find(l => l.startsWith('var LZString='));
const cfgLine = html.find(l => l.startsWith('var config = '));
const dataLine = html.find(l => l.startsWith('var pcbdata = '));
if (!lzLine || !dataLine) throw new Error('pcbdata not found in ' + BOM);

eval(lzLine);
eval(cfgLine);
eval(dataLine);

fs.writeFileSync(path.join(__dirname, 'pcbdata.json'), JSON.stringify(pcbdata, null, 1));
console.log('metadata:', JSON.stringify(pcbdata.metadata));
console.log('footprints:', pcbdata.footprints.length);
