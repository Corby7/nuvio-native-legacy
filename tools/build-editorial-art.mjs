// Editable vector masters + 4K PNGs for SDL_image on webOS 4.
// Run: node tools/build-editorial-art.mjs [--svg-only]
// The catalog is read for identity only; private source URLs are never copied.
import fs from 'node:fs';
import path from 'node:path';
import {createRequire} from 'node:module';
const require=createRequire(import.meta.url);
const sharp=process.argv.includes('--svg-only')?null:require('sharp');
const root=path.resolve(import.meta.dirname,'..');
const masters=path.join(root,'assets/editorial');
const output=path.join(root,'deploy/app/art/editorial');
fs.mkdirSync(masters,{recursive:true});fs.mkdirSync(output,{recursive:true});
const catalog=JSON.parse(fs.readFileSync(path.join(root,'deploy/app/art/collections.json'),'utf8'));
const rect=(x,y,w,h,fill='none',extra='')=>`<rect x="${x}" y="${y}" width="${w}" height="${h}" fill="${fill}" ${extra}/>`;
const circle=(x,y,r,fill='none',extra='')=>`<circle cx="${x}" cy="${y}" r="${r}" fill="${fill}" ${extra}/>`;
const p=(d,fill='none',extra='')=>`<path d="${d}" fill="${fill}" ${extra}/>`;
const line=(x,y,X,Y)=>p(`M${x} ${y}L${X} ${Y}`);
const repeat=(n,f)=>Array.from({length:n},(_,i)=>f(i)).join('');
const group=(body,transform='',extra='')=>`<g transform="${transform}" ${extra}>${body}</g>`;
const star=(x,y,r)=>p(repeat(10,i=>`${i?'L':'M'}${x+Math.sin(i*Math.PI/5)*(i%2?r*.42:r)} ${y-Math.cos(i*Math.PI/5)*(i%2?r*.42:r)}`)+'Z','currentColor');
const film=(x,y,w,h)=>rect(x,y,w,h,'#141719')+repeat(Math.floor(w/24),i=>rect(x+8+i*24,y+7,10,9,'currentColor')+rect(x+8+i*24,y+h-16,10,9,'currentColor'))+repeat(Math.floor(w/110),i=>rect(x+16+i*110,y+28,90,h-56,'#202326'));
const laurel=(x,y,s=1)=>group(repeat(10,i=>group(p('M0 0Q-32 -35 -12 -48Q12 -30 0 0Z','currentColor'),`translate(${i*7} ${-i*21}) rotate(${i*4-25})`))+p('M0 10Q20 -110 90 -220'),`translate(${x} ${y}) scale(${s})`);
const stairs=(x,y,n=9)=>group(repeat(n,i=>p(`M${i*18} ${-i*14}l100 -28 0 14 -100 28Z`,'#30383d')+line(i*18,-i*14,i*18,-i*14+14)),`translate(${x} ${y})`);
const buildings=(seed=0)=>repeat(13,i=>{const h=55+(i*47+seed*31)%175;return rect(i*49,310-h,40,h,'#202529')+repeat(Math.floor(h/23),j=>line(i*49+9,320-h+j*23,i*49+29,320-h+j*23));});
const bird=(x,y,s=1)=>group(p('M-90 10Q-50 -75 0 -3Q40 -50 95 -25Q45 -6 12 22L-2 40 -12 19Q-55 -7 -90 10Z','currentColor'),`translate(${x} ${y}) scale(${s})`);
const spiral=(x,y)=>p(repeat(240,i=>{const t=i*.12,r=i*.68;return `${i?'L':'M'}${x+Math.cos(t)*r} ${y+Math.sin(t)*r}`;}));
const arch=(x,y,w,h)=>p(`M${x} ${y+h}V${y+w/2}a${w/2} ${w/2} 0 0 1 ${w} 0V${y+h}Z`,'#202326');
const orbit=(x,y)=>repeat(4,i=>circle(x,y,45+i*36))+repeat(16,i=>group(line(0,-185,0,-175),`translate(${x} ${y}) rotate(${i*22.5})`));
const top=(x,y,s=1)=>group(
  p('M-5 -68Q-11 -89 0 -91Q11 -89 5 -68L8 -27Q15 -10 64 7Q77 12 69 19Q29 37 0 88Q-29 37 -69 19Q-77 12 -64 7Q-15 -10 -8 -27Z','#46535b')+
  p('M-69 16Q0 46 69 16M-55 22Q0 42 55 22M-8 -27Q0 -19 8 -27M-64 7Q0 29 64 7')+
  p('M0 34V88M-3 -74V-38','none','stroke-opacity=".45"'),`translate(${x} ${y}) scale(${s})`);
// Two related but genuinely different compositions per identity. Motifs are
// authored in a 760x430 artboard, never in the application's text column.
const designs={
 'Christopher Nolan':['#9fadb5',d=>d?
   orbit(410,215)+top(410,212,1.05)+stairs(55,360,11):
   repeat(5,i=>rect(100+i*42,18+i*28,480-i*56,325-i*34,'#171c20'))+stairs(165,355,12)+circle(540,135,84)+top(540,135,.83)],
 'Quentin Tarantino':['#b99954',d=>d?
   group(film(50,100,620,158),'rotate(-12 360 215)')+p('M195 315L645 35 660 33 659 49 209 330Z','#b99954'):
   group(rect(175,10,140,440,'#766034'),'rotate(27 245 215)')+group(film(10,220,670,142),'rotate(-9 350 290)')+p('M130 65L620 310 595 307 120 84Z','#d2c0a1')],
 'Steven Spielberg':['#9fbac1',d=>d?
   repeat(6,i=>p(`M${30+i*75} 400L375 28 ${730-i*65} 400`))+circle(375,125,53,'#607985')+p('M350 320L400 320 410 350 340 350Z','#222d33'):
   circle(435,180,140,'#637781')+circle(435,180,119,'#7c9096')+p('M15 375Q175 280 280 345T750 310V430H15Z','#171e22')+circle(394,223,27)+circle(476,223,27)+p('M394 223L428 180 449 223 394 223 466 189 476 223M428 180L445 178M439 178L429 149 408 143')+circle(426,132,10,'currentColor')],
 'Martin Scorsese':['#b68a70',d=>d?
   group(film(40,140,660,150),'rotate(11 370 215)')+group(buildings(5),'translate(80 -80) scale(.8)'):
   buildings(2)+repeat(9,i=>line(10+i*75,430,330+i*13,310))+circle(533,104,61,'#6a4d43')],
 'David Fincher':['#9caa8d',d=>d?
   repeat(8,i=>rect(110+i*22,30+i*20,485-i*44,350-i*40))+p('M352 44V165H470V295H262V372','none','stroke-width="6"'):
   repeat(7,i=>p(`M${80+i*24} ${40+i*25}H${650-i*25}V${370-i*23}H${130+i*24}V${100+i*25}`))+circle(415,209,37,'#62705d')],
 'Denis Villeneuve':['#bd9670',d=>d?
   repeat(5,i=>p(`M30 ${330+i*20}Q250 ${40+i*27} 730 ${250+i*27}`))+circle(495,123,77,'#ad825b')+circle(520,103,78,'#0d0d0d'):
   circle(485,133,96,'#b79168')+repeat(5,i=>p(`M0 ${370+i*22}Q190 ${130+i*45} 410 ${240+i*25}T780 ${285+i*28}V450H0Z`,i%2?'#272723':'#39362e'))+line(543,275,543,316)],
 'Stanley Kubrick':['#b0a09a',d=>d?
   circle(390,212,169)+circle(390,212,124)+circle(390,212,68,'#653d36')+circle(390,212,35,'#b39175')+repeat(12,i=>group(line(0,130,0,170),`translate(390 212) rotate(${i*30})`)):
   repeat(8,i=>p(`M${i*65} 425L340 185 430 185 ${760-i*65} 425`))+rect(322,38,105,280,'#171819','stroke-width="3"')+line(322,38,350,22)+line(350,22,454,22)+p('M427 38L454 22V300L427 318Z','#363134')],
 'Alfred Hitchcock':['#b8aba0',d=>d?
   bird(375,207,1.65)+bird(160,104,.48)+bird(621,127,.6)+repeat(7,i=>line(90,315+i*12,675-i*45,315+i*12)):
   spiral(395,215)+bird(585,83,.62)+bird(213,335,.5)],
 'Wes Anderson':['#bfa08b',d=>d?
   rect(90,142,580,173,'#594744')+repeat(6,i=>arch(117+i*88,163,56,104))+rect(72,124,615,18,'#80675b')+circle(201,325,22,'#292626')+circle(560,325,22,'#292626'):
   rect(230,97,310,260,'#66514c')+rect(285,53,200,44,'#85645b')+p('M264 53L385 5 507 53Z','#9c7968')+repeat(3,j=>repeat(5,i=>arch(249+i*58,117+j*65,29,48)))+arch(353,280,64,77)+rect(171,180,59,177,'#514440')+rect(540,180,59,177,'#514440')],
 'Tim Burton':['#a29aad',d=>d?
   spiral(410,172)+p('M95 388Q155 130 267 325Q315 160 352 392','#282331')+circle(557,108,62,'#6d6679'):
   circle(486,144,112,'#676374')+p('M130 395L169 180 189 184 174 142 215 167 244 151 228 190 249 350 288 263 320 391Z','#23202c')+p('M264 399Q328 209 444 279Q540 337 458 338Q408 337 435 307')],
 'Coen Brothers':['#a7b5b3',d=>d?
   p('M55 330Q230 290 725 315L740 430H40Z','#414b4c')+line(140,55,140,337)+line(85,100,198,100)+p('M140 120Q420 220 680 144')+circle(515,168,40):
   p('M50 420L328 93 430 93 720 420Z','#303638')+line(375,110,375,157)+line(375,190,375,259)+line(375,305,375,405)+p('M15 320L145 210 241 268 326 120M430 121L566 285 683 178 748 289')],
 'Guillermo del Toro':['#9ca98a',d=>d?
   circle(390,146,44)+rect(381,190,18,150,'#6e785e')+p('M399 303H445V325H420V342H389')+laurel(243,365,.8)+group(laurel(243,365,.8),'translate(780 0) scale(-1 1)'):
   p('M377 218Q170 12 120 160Q153 248 343 245Q185 289 236 355Q347 381 377 238M389 218Q596 12 646 160Q613 248 423 245Q581 289 530 355Q419 381 389 238','#3c493d')+p('M383 170Q365 232 383 294Q403 230 383 170Z','#a0aa88')+repeat(3,i=>circle(248+i*22,184+i*7,45-i*12))+repeat(3,i=>circle(518-i*22,184+i*7,45-i*12))],
 'IMDb Top 250 Movies':['#b4985d',d=>d?
   group(film(45,70,650,135),'rotate(-14 375 180)')+group(film(90,245,565,120),'rotate(10 375 300)')+star(415,222,62):
   laurel(195,376)+group(laurel(195,376),'translate(780 0) scale(-1 1)')+star(390,190,76)+group(film(55,296,660,108),'rotate(-5 385 350)')],
 'IMDb Top 250 Shows':['#ac965f',d=>d?
   repeat(4,i=>group(film(90+i*125,80,113,260),`translate(0 ${i%2?45:0})`))+star(388,67,30):
   repeat(4,i=>rect(100+i*96,60+i*36,310,220,'#292a23'))+repeat(3,i=>line(425+i*26,211,425+i*26,278))+star(381,145,37)],
 'Letterboxd Top 250':['#9eaba5',d=>d?
   group(film(70,235,620,125),'rotate(-12 380 295)')+['#946439','#44745b','#426b83'].map((c,i)=>circle(242+i*130,142,80,c)).join(''):
   ['#946439','#44745b','#426b83'].map((c,i)=>circle(225+i*153,205,113,c)+circle(225+i*153,205,98)).join('')+repeat(6,i=>line(110+i*107,78,110+i*107,332))],
 'Criterion Collection':['#b2b7b6',d=>d?
   repeat(5,i=>arch(170+i*28,25+i*15,390-i*56,350-i*15))+circle(365,210,45):
   repeat(7,i=>circle(393,211,55+i*22))+rect(395,50,240,320,'#0d0d0d','stroke="none"')+group(film(388,148,295,127),'rotate(-9 400 211)')],
 'AFI Top 100':['#b0a08a',d=>d?
   repeat(5,i=>group(rect(235,60,270,290),`rotate(${i*9-18} 370 350)`))+star(370,180,65):
   repeat(7,i=>group(line(0,-150,0,-195),`translate(383 217) rotate(${i*30-90})`))+star(383,217,111)+laurel(200,391,.7)+group(laurel(200,391,.7),'translate(766 0) scale(-1 1)')],
 '1001 Movies':['#b1a18d',d=>d?
   repeat(9,i=>group(rect(70+i*69,91,49,244,'#37332d'),`rotate(${i%3-1} ${90+i*69} 330)`))+line(58,349,720,349):
   p('M90 95Q235 56 380 132Q525 56 670 95V351Q525 312 380 388Q235 312 90 351Z','#39352e')+line(380,132,380,388)+repeat(7,i=>p(`M113 ${130+i*30}Q241 ${97+i*30} 355 ${160+i*30}M405 ${160+i*30}Q530 ${97+i*30} 649 ${130+i*30}`))],
 'Oscars: Best Picture Winners':['#c0a270',d=>d?
   laurel(220,393,1.2)+group(laurel(220,393,1.2),'translate(780 0) scale(-1 1)')+star(390,153,60)+rect(290,342,200,25,'#82704d'):
   circle(390,91,25,'#a88b58')+p('M370 123L354 230 375 237 373 326H409L405 237 426 230 410 123Z','#a88b58')+rect(339,327,104,23,'#726044')+rect(318,350,146,28,'#484032')+circle(390,190,164)],
 'Oscars: Best Picture Nominees':['#9e9179',d=>d?
   repeat(5,i=>star(160+i*115,210+(i%2)*65,37))+laurel(98,367,.6)+group(laurel(98,367,.6),'translate(780 0) scale(-1 1)'):
   repeat(5,i=>group(rect(210,90,340,250,'#242521'),`rotate(${i*10-20} 380 340)`))+star(380,195,63)],
 'Best Movies of 2024':['#bda17e',d=>d?
   group(film(50,130,650,170),'rotate(-11 375 215)')+circle(567,95,47,'#826449'):
   repeat(4,i=>group(rect(230,75,270,290,'#232521'),`rotate(${i*14-21} 370 355)`))+circle(373,191,72,'#8a7051')+p('M343 147L411 191 343 235Z','#242622')],
 'Best Movies of 2025':['#95aeb6',d=>d?
   repeat(5,i=>rect(90+i*119,80+i*22,93,220,'#2f4149'))+circle(388,190,64):
   circle(392,211,147)+group(film(50,110,650,190),'rotate(14 375 215)')+star(390,209,58)],
 'Best Shows of 2025':['#ab9bb7',d=>d?
   repeat(5,i=>group(rect(100+i*111,95,95,230,'#3a3342'),`translate(0 ${i%2?35:0})`))+repeat(5,i=>circle(147+i*111,215+(i%2?35:0),17)):
   repeat(5,i=>rect(95+i*83,40+i*36,345,226,'#29262f'))+p('M480 203L533 237 480 271Z','#a394af')]
};
const inventory=[];
for(const g of catalog.groups.filter(g=>['Directors','Awards'].includes(g.title)))for(const f of g.folders){
  const design=designs[f.title];if(!design)throw new Error(`Missing artwork: ${f.title}`);
  const [accent,draw]=design;
  for(const variant of ['home','detail']){
    const detail=variant==='detail',h=detail?320:500;
    const scale=detail?.72:1.02,tx=detail?1210:1080,ty=detail?0:12;
    const svg=`<svg xmlns="http://www.w3.org/2000/svg" width="3840" height="${h*2}" viewBox="0 0 1920 ${h}">
<title>${f.title.replaceAll('&','&amp;')} — ${variant}</title>
<defs><linearGradient id="fade" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#0d0d0d" stop-opacity="0"/><stop offset=".65" stop-color="#0d0d0d" stop-opacity="0"/><stop offset="1" stop-color="#0d0d0d"/></linearGradient></defs>
${rect(0,0,1920,h,'#0d0d0d')}
<g color="${accent}" stroke="${accent}" stroke-width="1.2" stroke-linejoin="round" stroke-linecap="round" opacity=".9" transform="translate(${tx} ${ty}) scale(${scale})">${draw(detail)}</g>
${rect(0,0,1920,h,'url(#fade)','stroke="none"')}
</svg>`;
    const name=`${f.id}-${variant}`,source=path.join(masters,`${name}.svg`);
    fs.writeFileSync(source,svg);
    if(sharp)await sharp(Buffer.from(svg)).removeAlpha().png().toFile(path.join(output,`${name}.png`));
    inventory.push({id:f.id,title:f.title,variant,width:3840,height:h*2,source:`${name}.svg`,runtime:`${name}.png`});
  }
}
fs.writeFileSync(path.join(masters,'manifest.json'),JSON.stringify(inventory,null,2)+'\n');
console.log(`${inventory.length} unique illustrations exported (${Object.keys(designs).length} identities).`);
