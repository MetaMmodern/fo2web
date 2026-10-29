import fs from 'node:fs';
const f=fs.openSync(process.argv[2],'r'),h=Buffer.alloc(128);fs.readSync(f,h,0,128,0);const e=h.indexOf(10),m=Buffer.alloc(+h.toString('utf8',0,e).split(/\s+/)[2]);fs.readSync(f,m,0,m.length,e+1);const d=JSON.parse(m),o=d.objects,base=(e+1+m.length+7)&~7;
function payload(id){const [off,len]=d.payloadTable[id],b=Buffer.alloc(len);fs.readSync(f,b,0,len,base+off);return b;}
const ent=d.commands.flatMap(c=>c.bufferData??[]).filter(b=>b.byteLength>1048576).sort((a,b)=>b.byteLength-a.byteLength)[0],v=payload(ent.__payloadId);
let p,groups=new Map(),target;const x=Number(process.argv[3]??0),y=Number(process.argv[4]??-.6);
for(const c of d.commands){
 if(c.method==='beginRenderPass')target=c.args[0].colorAttachments[0]?.view?.__id;
 if(c.method==='setPipeline')p=o[c.args[0].__id];if(c.method==='setBindGroup')groups.set(c.args[0],o[c.args[1].__id]);
 if(c.method!=='draw'||!p)continue;const vs=o[p.descriptor.vertex.module.__id];if(vs.label!=='vs_0')continue;
 const [n,,first]=c.args;let hits=0,example;
 for(let i=first;i<first+n;i+=3){const a=[];for(let k=0;k<3;k++){let off=(i+k)*112,w=v.readFloatLE(off+12);a.push([v.readFloatLE(off)/w,v.readFloatLE(off+4)/w,v.readFloatLE(off+8)/w,off,w]);}
 const [A,B,C]=a,den=(B[1]-C[1])*(A[0]-C[0])+(C[0]-B[0])*(A[1]-C[1]);if(!den)continue;
 const u=((B[1]-C[1])*(x-C[0])+(C[0]-B[0])*(y-C[1]))/den,t=((C[1]-A[1])*(x-C[0])+(A[0]-C[0])*(y-C[1]))/den,z=1-u-t;if(u<0||t<0||z<0)continue;hits++;
 const col=a.map(q=>[0,1,2,3].map(j=>v.readFloatLE(q[3]+16+j*4)));example={depth:u*A[2]+t*B[2]+z*C[2],col};}
 if(hits){const tex=(groups.get(1)?.descriptor.entries??[]).slice(0,4).map(q=>o[o[q.resource?.__id]?.texture?.__id]?.label);console.log(JSON.stringify({cmd:c.index,target,ps:o[p.descriptor.fragment.module.__id].label,n,hits,tex,depth:p.descriptor.depthStencil,blend:p.descriptor.fragment.targets[0].blend,...example}));}
}
