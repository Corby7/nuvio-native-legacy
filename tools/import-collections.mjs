// Read-only import from the owner's official Nuvio profile. Never logs URLs/tokens.
// Outputs private runtime data; does not alter the source application's settings.
import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
const root = path.resolve(import.meta.dirname, '..');
const art = path.join(root, 'deploy/app/art');
const source = process.argv[2] || '/Users/hrocha/Library/Application Support/Nuvio';
function property(file, key) {
  const line = fs.readFileSync(path.join(source, file+'.properties'), 'utf8').split('\n').find(l=>l.startsWith(key+'='));
  if (!line) throw Error('Profile property missing: '+key);
  return JSON.parse(line.slice(line.indexOf('=')+1).replace(/\\u([a-f\d]{4})/gi,(_,v)=>String.fromCharCode(parseInt(v,16))).replace(/\\([:=#! ])/g,'$1'));
}
const collections = property('nuvio_collections','collections_1');
const bases = fs.readFileSync(path.join(art,'addons.txt'),'utf8').split('\n').filter(l=>l&&!l.startsWith('#')).map(l=>l.split('\t')[1]?.trim().replace(/\/manifest\.json$/,'')).filter(Boolean);
const addons = new Map();
for (const base of bases) {
  try {const r=await fetch(base+'/manifest.json',{signal:AbortSignal.timeout(12000)});const m=await r.json();if(m.id) addons.set(m.id,{base,catalogs:m.catalogs||[]});} catch { /* inaccessible addon remains unresolved, never guessed */ }
}
const groups=[];
async function localImage(url,dir,id,kind) {
  if(!url) return '';
  const logo=kind==='logo', ext=logo?'png':'jpg';
  const rel=`collections/${id}/${kind}.${ext}`, out=path.join(art,rel);
  if(fs.existsSync(out)&&fs.statSync(out).size>256) return rel;
  fs.mkdirSync(dir,{recursive:true});
  const input=path.join(dir,`${kind}.input`);
  try {
    const response=await fetch(url,{signal:AbortSignal.timeout(15000)});
    if(!response.ok) throw Error('image');
    fs.writeFileSync(input,Buffer.from(await response.arrayBuffer()));
    const size=kind==='hero'?'1920:1080':kind==='portrait'?'460:690':'720:405';
    const vf=logo?'scale=800:300:force_original_aspect_ratio=decrease':`scale=${size}:force_original_aspect_ratio=increase,crop=${size}`;
    const args=['-v','error','-i',input,'-frames:v','1','-vf',vf];
    if(!logo) args.push('-q:v','4','-pix_fmt','yuvj420p');
    args.push('-y',out);
    const result=spawnSync('ffmpeg',args,{timeout:30000});
    if(result.status!==0) throw Error('convert');
    return rel;
  } catch { return url; }
  finally { try { fs.unlinkSync(input); } catch {} }
}
for (const c of collections) {
  const folders=[];
  for (const f of c.folders||[]) {
    const sources=[];
    for(const s of f.sources?.length?f.sources:f.catalogSources||[]) {
      if(s.provider && s.provider!=='addon') continue;
      const a=addons.get(s.addonId); const base=s.addonBaseUrl||a?.base;
      if(!base || !s.catalogId || !s.type) continue;
      const declared=a?.catalogs.find(x=>x.id===s.catalogId&&x.type===s.type);
      sources.push({title:s.title||s.catalogName||declared?.name||s.catalogId,base,type:s.type,catId:s.catalogId,genre:s.genre&&s.genre!=='None'?s.genre:''});
    }
    if(!sources.length) continue;
    const id=f.id.replace(/[^a-zA-Z0-9_-]/g,'_');
    const dir=path.join(art,'collections',id);
    const cover=await localImage(f.coverImageUrl,dir,id,'cover');
    const hero=await localImage(f.heroBackdropUrl||c.backdropImageUrl,dir,id,'hero');
    const logo=await localImage(f.titleLogoUrl,dir,id,'logo');
    if(c.title==='Directors'&&f.titleLogoUrl) {
      const portraitUrl=f.titleLogoUrl.replace('.logo.webp','.webp');
      await localImage(portraitUrl,dir,id,'portrait');
    }
    let frames=0;
    // 15 fps keeps the original focus motion fluid on TV without involving the
    // video pipeline. Only the focused tile asks for these bounded frames.
    if(f.focusGifEnabled!==false && f.focusGifUrl && !process.argv.includes('--no-animation')) {
      fs.mkdirSync(dir,{recursive:true});
      try {
        const gif=path.join(dir,'source.gif');
        if(!fs.existsSync(gif) || process.argv.includes('--refresh-animation')) {
          const response=await fetch(f.focusGifUrl,{signal:AbortSignal.timeout(15000)});
          if(!response.ok) throw Error('image');
          fs.writeFileSync(gif,Buffer.from(await response.arrayBuffer()));
        }
        for(const name of fs.readdirSync(dir)) if(/^\d{3}\.jpg$/.test(name)) fs.unlinkSync(path.join(dir,name));
        const result=spawnSync('ffmpeg',['-v','error','-i',gif,'-t','6','-vf','fps=15,scale=480:270:force_original_aspect_ratio=decrease,pad=480:270:(ow-iw)/2:(oh-ih)/2','-q:v','4','-frames:v','90','-y',path.join(dir,'%03d.jpg')],{timeout:30000});
        if(result.status===0) frames=fs.readdirSync(dir).filter(x=>/^\d{3}\.jpg$/.test(x)).length;
      } catch { console.log('Static fallback:',f.title); }
    }
    folders.push({id,title:f.title,cover,hero,logo,hideTitle:f.hideTitle?1:0,frames,sources});
  }
  if(folders.length) groups.push({id:c.id,title:c.title,folders});
}
fs.writeFileSync(path.join(art,'collections.json'),JSON.stringify({groups}),{mode:0o600});
console.log('Imported',groups.length,'collections,',groups.reduce((n,c)=>n+c.folders.length,0),'folders; original settings untouched.');
