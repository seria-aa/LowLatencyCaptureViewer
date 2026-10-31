// Local, read-only DOM/image validation. Does not launch a browser or the viewer.
const fs=require('fs'),path=require('path'),vm=require('vm'),assert=require('assert');
const [folder,fragment,option]=process.argv.slice(2),soft=option==='--soft';
const script=html=>html.match(/<script>([\s\S]*?)<\/script>/)[1];
const element=()=>({value:'',dataset:{},options:[],attributes:{},listeners:{},
 setAttribute(k,v){this.attributes[k]=v;},addEventListener(k,f){this.listeners[k]=f;},
 appendChild(o){this.options.push(o);if(!this.value)this.value=o.value;},replaceChildren(){this.options=[];this.value='';},
 get selectedIndex(){return this.options.findIndex(o=>o.value===this.value);}});
const select=values=>{const x=element();x.options=values.map(([value,text])=>({value,text,textContent:text}));x.value=values[0][0];return x;};
const methods=soft?['off','nis','cmaa-preserve','dlaa','dlaa50','dlaa25','reference']:['off','nis','cmaa-current','dlaa','fxaa','reference'];
let calls=0;
const nodes={scene:select([['antialiased','smooth'],['aliased','jagged'],['game','game']]),resolution:select([['2560','QHD'],['3840','4K']]),region:element(),caption:element(),original:element(),picture:element()};
const buttons=methods.map(mode=>Object.assign(element(),{dataset:{mode}}));
nodes.picture.getContext=()=>({imageSmoothingEnabled:false,drawImage(image,x,y,w,h,dx,dy,dw,dh){assert(x>=0&&y>=0&&x+w<=image.width&&y+h<=image.height);assert(w===dw&&h===dh);calls++;}});
class PngImage{set src(relative){const p=path.join(folder,relative);assert(fs.existsSync(p),relative);const bytes=fs.readFileSync(p);this.width=bytes.readUInt32BE(16);this.height=bytes.readUInt32BE(20);this.onload();}}
const document={getElementById:id=>{assert(id in nodes,id);return nodes[id];},querySelectorAll:()=>buttons,createElement:()=>({})};
vm.runInNewContext(script(fs.readFileSync(path.join(folder,'comparison.html'),'utf8')),{document,Image:PngImage});
let combinations=0;
for(const source of ['antialiased','aliased','game']){
 nodes.scene.value=source;nodes.scene.listeners.change();
 for(const width of ['2560','3840']){nodes.resolution.value=width;nodes.resolution.listeners.change();
  for(const region of nodes.region.options){nodes.region.value=region.value;
   for(const b of buttons){if(b.disabled)continue;b.listeners.click();assert(b.attributes['aria-pressed']==='true');assert(nodes.original.href.includes(b.dataset.mode+'.png'));combinations++;}
  }
 }
}
const inline=fs.readFileSync(fragment,'utf8');assert(Buffer.byteLength(inline)<1000000);assert(!/<!doctype|<html|<head|<body/i.test(inline));
const source=select([['antialiased','smooth'],['aliased','jagged'],['game','game']]),picture=element(),caption=element();
const controls=buttons.filter(b=>!(soft?['off','reference','dlaa']:['off','reference']).includes(b.dataset.mode));let saves=0;
const root={querySelector:s=>({'[data-source]':source,'[data-picture]':picture,'[data-caption]':caption}[s]),querySelectorAll:()=>controls};
const globals={};const window={openai:{widgetState:null,setWidgetState:()=>{saves++;return Promise.resolve();}},addEventListener:(key,fn)=>{globals[key]=fn;}};
vm.runInNewContext(script(inline),{document:{getElementById:id=>{assert(id===(soft?'llcv-soft-dlaa-comparison':'llcv-spatial-aa-candidates'));return root;}},window});
assert(saves===0);let inlineCases=0;
for(const scene of source.options){source.value=scene.value;source.listeners.change();for(const b of controls){b.listeners.click();const bytes=Buffer.from(picture.src.split(',')[1],'base64');assert(bytes.subarray(1,4).toString()==='PNG');assert(picture.alt.includes(scene.text));assert(b.attributes['aria-pressed']==='true');inlineCases++;}}
const before=saves;globals['openai:set_globals']({detail:{globals:{widgetState:{privateContent:{source:'aliased',mode:soft?'dlaa25':'fxaa'}}}}});assert(saves===before);assert(source.value==='aliased');assert(caption.textContent.includes(soft?'25%':'FXAA'));
console.log(JSON.stringify({standaloneCombinations:combinations,canvasDraws:calls,inlineCombinations:inlineCases,inlineBytes:Buffer.byteLength(inline),stateRestoration:true},null,2));
