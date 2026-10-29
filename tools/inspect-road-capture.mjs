import fs from 'node:fs';
const f=fs.openSync(process.argv[2],'r'),h=Buffer.alloc(128);fs.readSync(f,h,0,128,0);
const e=h.indexOf(10),m=Buffer.alloc(+h.toString('utf8',0,e).split(/\s+/)[2]);fs.readSync(f,m,0,m.length,e+1);
const d=JSON.parse(m),o=d.objects;let p,groups=new Map(),seen=new Set();
if(process.argv.includes('--draws')){
 let target;
 for(const c of d.commands){
 if(c.method==='beginRenderPass')target=c.args[0].colorAttachments[0]?.view?.__id;
 if(c.method==='setPipeline')p=o[c.args[0].__id];
 if(c.method==='setBindGroup')groups.set(c.args[0],o[c.args[1].__id]);
 if(!['draw','drawIndexed'].includes(c.method)||!p)continue;
 const tex=(groups.get(1)?.descriptor.entries??[]).slice(0,4).map(q=>o[o[q.resource?.__id]?.texture?.__id]?.label);
 console.log(JSON.stringify({cmd:c.index,target,args:c.args,vs:o[p.descriptor.vertex.module.__id]?.label,ps:o[p.descriptor.fragment.module.__id]?.label,tex,depth:p.descriptor.depthStencil,groups:[...groups].map(([n,g])=>[n,g.id])}));
 }process.exit();
}
for(const c of d.commands){
 if(c.method==='beginRenderPass')console.log('PASS',c.index,JSON.stringify(c.args));
 if(c.method==='setPipeline')p=o[c.args[0].__id];
 if(c.method==='setBindGroup')groups.set(c.args[0],o[c.args[1].__id]);
 if(!['draw','drawIndexed'].includes(c.method)||!p)continue;
 const s=o[p.descriptor.fragment.module.__id],key=s.label;
 if(seen.has(key))continue;seen.add(key);
 console.log('SHADER',key,'first draw',c.index,'pipeline',JSON.stringify(p.descriptor));
 console.log(s.descriptor.code);
 console.log('GROUPS',JSON.stringify([...groups]));
}
