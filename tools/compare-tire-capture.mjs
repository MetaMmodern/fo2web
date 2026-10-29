import fs from 'node:fs';
import path from 'node:path';
const f=fs.openSync(process.argv[2],'r'), h=Buffer.alloc(128);
fs.readSync(f,h,0,128,0);
const e=h.indexOf(10), m=Buffer.alloc(+h.toString('utf8',0,e).split(/\s+/)[2]);
fs.readSync(f,m,0,m.length,e+1);
const d=JSON.parse(m), base=(e+1+m.length+7)&~7;
function payload(id){const [off,len]=d.payloadTable[id],b=Buffer.alloc(len);fs.readSync(f,b,0,len,base+off);return b;}
const entries=d.commands.flatMap(c=>c.bufferData??[]).filter(b=>b.byteLength>1048576).sort((a,b)=>b.byteLength-a.byteLength);
const v=payload(entries[0].__payloadId);
const pairs=[];
for(let i=29658;i<34290;i++)pairs.push(v.subarray(i*112+48,i*112+56));
const root=process.argv[3];
for(const name of fs.readdirSync(root).filter(n=>/^tire.*\.bgm$/i.test(n))){
 const b=fs.readFileSync(path.join(root,name));let matches=0;
 for(const uv of pairs)if(b.indexOf(uv)>=0)matches++;
 console.log(name,`${matches}/${pairs.length} captured UV pairs found byte-exact in model`);
}
const tex=d.objects[1717], bytes=payload(tex.mipData[0].__payloadId);
const blockOffsets=new Map();
for(let i=0;i<bytes.length;i+=16){const k=bytes.subarray(i,i+16).toString('hex');if(!blockOffsets.has(k))blockOffsets.set(k,i);}
console.log('texture payload',bytes.length,'first32',bytes.subarray(0,32).toString('hex'));
for(const name of fs.readdirSync(root).filter(n=>/\.dds$/i.test(n))){
 const b=fs.readFileSync(path.join(root,name));
 if(b.toString('ascii',84,88)==='DXT3'){
 const w=b.readUInt32LE(16),ht=b.readUInt32LE(12),len=Math.ceil(w/4)*Math.ceil(ht/4)*16;
 if(bytes.subarray(0,len).equals(b.subarray(128,128+len)))console.log('EXACT TOP MIP',name,w,ht,len,'bytes');
 if(name==='tire_01.dds'){
 let exactRows=0;for(let row=0;row<32;row++)if(bytes.subarray(row*4096,row*4096+1024).equals(b.subarray(128+row*1024,128+(row+1)*1024)))exactRows++;
 console.log('EXACT tire_01 TOP-MIP ROWS at capture pitch4096',exactRows,'/32');
 for(let row=0;row<32;row++){let matched=-1;for(let col=0;col<64;col++){let off=128+row*1024+col*16,k=b.subarray(off,off+16).toString('hex');if(blockOffsets.has(k)&&blockOffsets.get(k)!==0){matched=blockOffsets.get(k)-col*16;break;}}console.log('ROW',row,'capture row base',matched);}
 let twice=true;for(let row=0;row<32;row++)for(let rep=0;rep<2;rep++)if(!bytes.subarray((row*2+rep)*1024,(row*2+rep+1)*1024).equals(b.subarray(128+row*1024,128+(row+1)*1024)))twice=false;
 console.log('ALL BC2 BLOCK ROWS DUPLICATED VERTICALLY',twice);
 }
 let unique=new Set(),hits=0,examples=[];
 for(let off=128;off<128+len&&off+16<=b.length;off+=16){let k=b.subarray(off,off+16).toString('hex');if(unique.has(k))continue;unique.add(k);if(blockOffsets.has(k)){hits++;if(examples.length<6)examples.push([off-128,blockOffsets.get(k)]);}}
 if(hits>20)console.log('BLOCK MATCH',name,w,ht,hits,'/',unique.size,JSON.stringify(examples));
 }
 if(b.length>=128+65536 && bytes.subarray(0,65536).equals(b.subarray(128,128+65536)))console.log('EXACT DDS BC2 match',name);
 if(name==='tire_08.dds'){
  console.log('expected tire_08 header',b.subarray(84,88).toString(), 'size',b.length);
  let same=0;for(let row=0;row<64;row++)if(bytes.subarray(row*4096,row*4096+1024).equals(b.subarray(128+row*1024,128+(row+1)*1024)))same++;
  console.log('tire_08 matching BC2 rows with capture pitch4096',same,'of64');
  for(let row of [0,8,16,32]){const needle=b.subarray(128+row*1024,128+row*1024+128);console.log('DDS row',row,'found at capture offset',bytes.indexOf(needle));}
 }
}
