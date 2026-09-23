const fs=require('fs');
const path=require('path');
const GDIR = require('path').join(__dirname, '..', '..', 'MSX_PicoVerse_2350_1.2');
const ib=JSON.parse(fs.readFileSync('pcbdata.json','utf8'));
const cu=JSON.parse(fs.readFileSync('cu.json','utf8'));
const pads=JSON.parse(fs.readFileSync('pads.json','utf8'));

// ---- 1. footprint pad naming (match by position, y-flip) ----
const gpByRef={};
[...cu.F.pads,...cu.B.pads].filter(p=>p.P).forEach(p=>{(gpByRef[p.P[0]]=gpByRef[p.P[0]]||[]).push(p)});
const fps=[];
for(const fp of ib.footprints){
  if(!fp.pads.length) continue;
  const g=gpByRef[fp.ref]||[];
  const out={ref:fp.ref, layer:fp.layer, bbox:fp.bbox, pads:[]};
  fp.pads.forEach(pad=>{
    let best=null,bd=1e9;
    g.forEach(q=>{
      if(pad.type==='smd' && pad.layers.length===1 && q.layer!==pad.layers[0]) return;
      const d=Math.hypot(q.x-pad.pos[0],(-q.y)-pad.pos[1]); if(d<bd){bd=d;best=q;}});
    const ok = best && bd<0.05;
    out.pads.push({
      pos:pad.pos, size:pad.size, angle:pad.angle, shape:pad.shape, radius:pad.radius,
      type:pad.type, drillshape:pad.drillshape, drillsize:pad.drillsize, layers:pad.layers,
      polygons:pad.polygons, offset:pad.offset, chamfpos:pad.chamfpos, chamfratio:pad.chamfratio,
      name: ok? best.P[1] : null,
      net:  ok? best.net  : null,
      fn:   ok? best.P.slice(2).join(',') : null
    });
  });
  fps.push(out);
}

// ---- 2. NPTH holes ----
function parseDrill(file){
  const txt=fs.readFileSync(path.join(GDIR,file),'utf8').split(/\r?\n/);
  const tools={}; let cur=null; const holes=[]; let lastFn=null;
  txt.forEach(L=>{L=L.trim(); let m;
    if(m=L.match(/^; #@! TA\.AperFunction,(.*)$/)){ lastFn=m[1]; return; }
    if(m=L.match(/^T(\d+)C([\d.]+)$/)){ tools['T'+m[1]]={dia:+m[2], fn:lastFn}; return; }
    if(m=L.match(/^T(\d+)$/)){ cur='T'+m[1]; return; }
    if(m=L.match(/^X(-?[\d.]+)Y(-?[\d.]+)$/)){ holes.push({x:+m[1], y:-(+m[2]), dia:tools[cur].dia, fn:tools[cur].fn}); return; }
  });
  return holes;
}
const npth=parseDrill('msx-picoverse-NPTH.drl');
const pth =parseDrill('msx-picoverse-PTH.drl');

// ---- 3. vias : PTH holes with ViaDrill function, matched to flash for pad size ----
const flashes=[...cu.F.pads,...cu.B.pads];
const vias=pth.filter(h=>/ViaDrill/.test(h.fn||'')).map(h=>{
  // find a flash at that position to get the annular ring & net
  let best=null,bd=1e9;
  flashes.forEach(q=>{const d=Math.hypot(q.x-h.x,(-q.y)-h.y); if(d<bd){bd=d;best=q;}});
  const ap = best && bd<0.02 ? cu.F.apertures[best.aper]||cu.B.apertures[best.aper] : null;
  return {x:h.x, y:h.y, drill:h.dia, size: ap&&ap.type==='C'? ap.params[0] : h.dia+0.3, net: best&&bd<0.02? best.net : null};
});

// ---- 4. tracks ----
function conv(t,layer){
  const o={x1:t.x1,y1:-t.y1,x2:t.x2,y2:-t.y2,layer,net:t.net,width:null,arc:null};
  return o;
}
function tracksOf(g,layer){
  return g.tracks.map(t=>{
    const ap=g.apertures[t.aper];
    const w= ap && ap.type==='C' ? ap.params[0] : 0.2;
    const o={x1:t.x1,y1:-t.y1,x2:t.x2,y2:-t.y2,layer,net:t.net,width:w};
    if(t.interp==='G02'||t.interp==='G03'){
      const cx=t.x1+t.i, cy=t.y1+t.j;
      o.arc={cx, cy:-cy, dir:t.interp};
    }
    return o;
  });
}
const tracks=[...tracksOf(cu.F,'F.Cu'), ...tracksOf(cu.B,'B.Cu')];

// ---- 5. edge cuts ----
function parseEdge(file){
  const txt=fs.readFileSync(path.join(GDIR,file),'utf8').split(/\r?\n/);
  let x=0,y=0,interp='G01',lastD=null; const segs=[];
  const SC=1e6;
  txt.forEach(L=>{L=L.trim(); let m;
    if(L.startsWith('G04')||L.startsWith('%')) return;
    if(L==='G01*'){interp='G01';return;}
    if(L==='G02*'){interp='G02';return;}
    if(L==='G03*'){interp='G03';return;}
    if(L==='G75*'||L==='G74*') return;
    if(m=L.match(/^(?:(G0[123]))?(?:X(-?\d+))?(?:Y(-?\d+))?(?:I(-?\d+))?(?:J(-?\d+))?(?:(D0[12]))?\*$/)){
      if(m[1])interp=m[1];
      const nx=m[2]!==undefined?(+m[2])/SC:x, ny=m[3]!==undefined?(+m[3])/SC:y;
      const d=m[6]||lastD; if(m[6])lastD=m[6];
      if(d==='D01'){
        if(interp==='G01') segs.push({t:'line',x1:x,y1:-y,x2:nx,y2:-ny});
        else { const cx=x+(m[4]!==undefined?(+m[4])/SC:0), cy=y+(m[5]!==undefined?(+m[5])/SC:0);
               segs.push({t:'arc',x1:x,y1:-y,x2:nx,y2:-ny,cx,cy:-cy,dir:interp}); }
      }
      x=nx;y=ny;
    }
  });
  return segs;
}
const edge=parseEdge('msx-picoverse-Edge_Cuts.gm1');

// netlist
const nets={};
pads.forEach(p=>{ const n=p.net||'unconn'; (nets[n]=nets[n]||[]).push({ref:p.ref,pin:p.pin,fn:p.fn}); });

fs.writeFileSync('model.json', JSON.stringify({fps,npth,pth,vias,tracks,edge,nets,bbox:ib.edges_bbox},null,1));
console.log('fps',fps.length,'npth',npth.length,'pth',pth.length,'vias',vias.length,'tracks',tracks.length,'edge',edge.length,'nets',Object.keys(nets).length);
console.log('arcs in tracks', tracks.filter(t=>t.arc).length);
console.log('edge arcs', edge.filter(e=>e.t==='arc').length);
console.log('via nets null', vias.filter(v=>!v.net).length);
