const fs=require('fs');
const ib=JSON.parse(fs.readFileSync('pcbdata.json','utf8'));
const cu=JSON.parse(fs.readFileSync('cu.json','utf8'));
// all gerber flashes with TO.P
const gp=[...cu.F.pads,...cu.B.pads].filter(p=>p.P);
const byref={};
gp.forEach(p=>{(byref[p.P[0]]=byref[p.P[0]]||[]).push(p)});
const report=[];
ib.footprints.forEach(fp=>{
  const g=byref[fp.ref]||[];
  fp.pads.forEach((pad,i)=>{
    // gerber y is flipped? check
    const cands=g.map(q=>({q,d:Math.hypot(q.x-pad.pos[0], (-q.y)-pad.pos[1])})).sort((a,b)=>a.d-b.d);
    pad._name = cands.length && cands[0].d<0.05 ? cands[0].q.P[1] : null;
    pad._net  = cands.length && cands[0].d<0.05 ? cands[0].q.net : null;
    pad._fn   = cands.length && cands[0].d<0.05 ? cands[0].q.P.slice(2).join(',') : null;
    pad._dist = cands.length? cands[0].d : 99;
  });
  const unmatched=fp.pads.filter(p=>!p._name).length;
  report.push(fp.ref+': pads='+fp.pads.length+' matched='+(fp.pads.length-unmatched));
});
console.log(report.join('\n'));
// dump J3 & SW1 mapping
['J3','SW1','J2','JP1','U2'].forEach(r=>{
  const fp=ib.footprints.find(f=>f.ref===r);
  console.log('--- '+r);
  fp.pads.forEach((p,i)=>console.log('  idx'+i, JSON.stringify(p.pos), '->', p._name, p._net, 'd='+p._dist.toFixed(3)));
});
fs.writeFileSync('matched.json', JSON.stringify(ib.footprints,null,1));
