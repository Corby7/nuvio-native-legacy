import fs from 'node:fs';
import path from 'node:path';
const url=process.argv[2];if(!url)throw Error('Usage: node tools/import-badges.mjs <pack.json>');
const response=await fetch(url,{signal:AbortSignal.timeout(15000)});if(!response.ok)throw Error('Badge pack unavailable');
const data=await response.json();const dir=path.resolve(import.meta.dirname,'../deploy/app/art/badges');fs.mkdirSync(dir,{recursive:true});
let n=0;const index=[];
for(const f of data.filters||[]) {
  if(!f.isEnabled||!f.imageURL||!/^[a-z0-9-]+$/.test(f.id))continue;
  try {
    const r=await fetch(f.imageURL,{signal:AbortSignal.timeout(10000)});if(!r.ok)throw Error('asset');
    fs.writeFileSync(path.join(dir,f.id+'.webp'),Buffer.from(await r.arrayBuffer()));
    index.push({id:f.id,name:f.name,image:f.id+'.webp'});n++;
  }catch{console.log('Unavailable badge:',f.id);}
}
fs.writeFileSync(path.join(dir,'index.json'),JSON.stringify({badges:index}));
console.log('Imported',n,'original badges. Matching uses native metadata, never title guesses.');
