const fs=require('fs');
const path=require('path');
const GDIR = require('path').join(__dirname, '..', '..', 'MSX_PicoVerse_2350_1.2');

function parseGerber(file, layer){
  const txt=fs.readFileSync(path.join(GDIR,file),'utf8');
  const lines=txt.split(/\r?\n/);
  // format spec
  let xi=4, xd=6;
  const apertures={};   // D-code -> {macro/type, params, aperFunction}
  const pads=[];        // flashes
  const tracks=[];
  const regions=[];
  let curAper=null, curTO_P=null, curTO_N=null, curTA=null;
  let x=0,y=0, interp='G01', inRegion=false, regionPts=[], lastD=null;
  const SC=Math.pow(10,xd);
  function num(s){ return parseInt(s,10)/SC; }
  for(let ln=0; ln<lines.length; ln++){
    let L=lines[ln].trim();
    if(!L) continue;
    let m;
    if(m=L.match(/^%FSLAX(\d)(\d)Y\d\d\*%$/)){ xi=+m[1]; xd=+m[2]; continue; }
    if(m=L.match(/^G04 #@! TO\.P,(.*)\*$/)){ curTO_P=m[1].split(','); continue; }
    if(m=L.match(/^G04 #@! TO\.N,(.*)\*$/)){ curTO_N=m[1]; continue; }
    if(m=L.match(/^G04 #@! TA\.AperFunction,(.*)\*$/)){ curTA=m[1]; continue; }
    if(L==='G04 #@! TD*'){ curTO_P=null; curTO_N=null; curTA=null; continue; }
    if(m=L.match(/^%AD(D\d+)([A-Za-z_][\w_]*),?(.*?)\*%$/)){
      apertures[m[1]]={type:m[2], params:m[3]?m[3].split('X').map(Number):[], func:curTA};
      continue;
    }
    if(m=L.match(/^%AD(D\d+)([CROP]),(.*)\*%$/)){ apertures[m[1]]={type:m[2],params:m[3].split('X').map(Number),func:curTA}; continue; }
    if(L.startsWith('G04')||L.startsWith('%')||L.startsWith('M02')) continue;
    if(L==='G36*'){ inRegion=true; regionPts=[]; continue; }
    if(L==='G37*'){ inRegion=false; if(regionPts.length) regions.push({pts:regionPts, net:curTO_N, layer}); continue; }
    if(L==='G01*'){ interp='G01'; continue; }
    if(L==='G02*'||L==='G03*'){ interp=L.slice(0,3); continue; }
    if(m=L.match(/^(D\d+)\*$/)){ curAper=m[1]; continue; }
    // coordinate lines
    if(m=L.match(/^(?:(G0[123]))?(?:X(-?\d+))?(?:Y(-?\d+))?(?:I(-?\d+))?(?:J(-?\d+))?(?:(D0[123]))?\*$/)){
      if(m[1]) interp=m[1];
      const nx = m[2]!==undefined ? num(m[2]) : x;
      const ny = m[3]!==undefined ? num(m[3]) : y;
      const d  = m[6] || lastD;
      if(m[6]) lastD=m[6];
      if(d==='D01'){
        if(inRegion) regionPts.push([nx,ny]);
        else tracks.push({x1:x,y1:y,x2:nx,y2:ny, aper:curAper, net:curTO_N, layer,
                          i: m[4]!==undefined?num(m[4]):0, j: m[5]!==undefined?num(m[5]):0, interp});
      } else if(d==='D02'){
        if(inRegion) regionPts.push([nx,ny]);
      } else if(d==='D03'){
        pads.push({x:nx,y:ny, aper:curAper, net:curTO_N, P:curTO_P, layer, func:apertures[curAper]&&apertures[curAper].func});
      }
      x=nx; y=ny;
      continue;
    }
    // unmatched
    if(!L.match(/^G\d+\*$/)) console.error('UNPARSED ['+file+':'+(ln+1)+']', L);
  }
  return {apertures, pads, tracks, regions};
}

const F=parseGerber('msx-picoverse-F_Cu.gtl','F');
const B=parseGerber('msx-picoverse-B_Cu.gbl','B');
fs.writeFileSync('cu.json', JSON.stringify({F,B},null,0));
console.log('F pads',F.pads.length,'tracks',F.tracks.length,'regions',F.regions.length);
console.log('B pads',B.pads.length,'tracks',B.tracks.length,'regions',B.regions.length);
console.log('F apertures', JSON.stringify(F.apertures,null,0).slice(0,1500));
