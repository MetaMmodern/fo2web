import fs from 'node:fs';
const d=JSON.parse(fs.readFileSync(process.argv[2],'utf8')),es=d.traceEvents??d;
const names=new Map(), totals=new Map(),samples=new Map();
for(const e of es){const k=`${e.pid}:${e.tid}`;if(e.name==='thread_name')names.set(k,e.args.name);if(e.ph==='X'&&e.dur){const key=`${k} ${e.name}`;totals.set(key,(totals.get(key)??0)+e.dur);}
 if(e.name==='ProfileChunk'){
 const p=e.args?.data?.cpuProfile;if(!p)continue;
 let st=samples.get(k);if(!st)samples.set(k,st={nodes:new Map(),counts:new Map()});
 for(const n of p.nodes??[])st.nodes.set(n.id,n);
 for(let i=0;i<(p.samples??[]).length;i++){const id=p.samples[i];st.counts.set(id,(st.counts.get(id)??0)+(e.args.data.timeDeltas?.[i]??1000));}
 }}
console.log('THREADS',Object.fromEntries(names));
console.log('EVENT TOTALS', [...totals].sort((a,b)=>b[1]-a[1]).slice(0,35).map(([k,v])=>[k,v/1000]));
for(const [k,st] of samples){const sums=new Map();for(const [id,n]of st.counts){const f=st.nodes.get(id)?.callFrame;const key=f?`${f.functionName} ${f.url}:${f.lineNumber}`:`node ${id}`;sums.set(key,(sums.get(key)??0)+n);}console.log('SAMPLES',k,names.get(k),[...sums].sort((a,b)=>b[1]-a[1]).slice(0,25).map(([f,n])=>[f,Math.round(n/1000)]));}
