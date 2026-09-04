// Validate pixels, source coverage and isolation, not implementation strings.
import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {createHash} from 'node:crypto';
const sharp=createRequire(import.meta.url)('sharp');
const manifest=JSON.parse(fs.readFileSync('assets/editorial/manifest.json','utf8'));
const catalog=JSON.parse(fs.readFileSync('deploy/app/art/collections.json','utf8'));
const folders=catalog.groups.filter(g=>['Directors','Awards'].includes(g.title)).flatMap(g=>g.folders);
assert.equal(manifest.length,folders.length*2);
const hashes=new Set();
for(const f of folders)for(const variant of ['home','detail']){
  const a=manifest.find(a=>a.id===f.id&&a.variant===variant);assert.ok(a,`${f.title}: ${variant}`);
  const file=`deploy/app/art/editorial/${a.runtime}`;
  const meta=await sharp(file).metadata();assert.equal(meta.width,a.width);assert.equal(meta.height,a.height);
  const hash=createHash('sha256').update(fs.readFileSync(file)).digest('hex');
  assert.ok(!hashes.has(hash),'Reused artwork');hashes.add(hash);
  const {data,info}=await sharp(file).removeAlpha().raw().toBuffer({resolveWithObject:true});
  let colored=0;
  for(let y=0;y<info.height;y+=8)for(let x=0;x<info.width;x+=8){
    const i=(y*info.width+x)*info.channels;
    const diff=Math.max(...data.subarray(i,i+3))-13;
    if(x<1920)assert.ok(Math.abs(diff)<=1,`${f.title}: art enters copy column`);
    if(diff>15)colored++;
  }
  assert.ok(colored>300,`${f.title}: blank/failed export`);
  const edge=await sharp(file).extract({left:0,top:info.height-2,width:info.width,height:2}).png().toBuffer();
  const bottom=await sharp(edge).stats();
  assert.ok(bottom.channels.every(c=>c.max<=15),'Art does not dissolve before content');
}
console.log(`editorial art: PASS (${folders.length} identities, ${hashes.size} distinct 4K images; clear copy and bottom)`);
