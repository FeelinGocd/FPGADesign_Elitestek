"use strict";
/* Run actual editor functions without boot/network. A scroll geometry model
 * clamps offsets like a browser, exposing one-sided zoom anchor failures. */
const fs = require("node:fs"), vm = require("node:vm"), assert = require("node:assert/strict");
const path = require("node:path");
const elements = {};
const doc = {
  getElementById(id) {
    return elements[id] ||= {
      style: {}, value: "", textContent: "", className: "",
      getContext() { return {}; }
    };
  }
};
const sandbox = { document: doc, location: {protocol:"http:"}, console };
const source = fs.readFileSync(path.join(__dirname, "app.js"), "utf8")
  .replace(/\n  boot\(\);/, `
  globalThis.editorTest = {S, images, placeDoor, removeDoorAt, paintDoor,
    arenaRect, doorSlots, serializeJson, parseJsonLocal, setZoom, onWheel,
    putCell, addObject, serializeExportJson, numberedFile};
  `);
assert(source.includes("globalThis.editorTest"));
vm.runInNewContext(source, sandbox);
const e = sandbox.editorTest;
function grid(c) {return Array.from({length:23},()=>Array(40).fill(c));}
function reset() {e.S.map=grid("w");e.S.rot=grid(0);e.S.cols=40;e.S.rows=23;}
for (let angle=0;angle<4;angle++) {
  reset();e.S.angle=angle;
  e.placeDoor(15,10,"H");
  assert.equal(e.S.map[10][15],"H");
  assert.equal(e.S.map[10][16],"w");
  assert.equal(e.S.map[11][15],"w");
  e.removeDoorAt(15,10);
  assert.equal(e.S.map[11][15],"w");
  assert.equal(e.S.map[10][16],"w");
  reset();e.S.angle=angle;
  e.placeDoor(15,10,"D");
  const px=15+(angle%2), py=10+(1-angle%2);
  assert.equal(e.S.map[py][px],".");
  e.removeDoorAt(px,py);
  assert.equal(e.S.map[10][15],".");
}
reset();e.S.angle=1;e.placeDoor(39,10,"H");e.placeDoor(39,10,"D");
assert.equal(e.S.map[10][39],"H","a rejected 2-cell door must not erase existing data");
e.images.wall={name:"wood",naturalWidth:32,naturalHeight:32};
e.images.hidden_wall={name:"hidden wall",naturalWidth:32,naturalHeight:32};
e.images.door={name:"D",naturalWidth:40,naturalHeight:64};
e.images.hide={name:"H",naturalWidth:32,naturalHeight:32};
const calls=[], translations=[];
const ctx={save(){},restore(){},rotate(){},scale(){},
  translate(x,y){translations.push([x,y]);},drawImage(img,...args){calls.push([img.name,...args]);}};
for(let angle=0;angle<4;angle++) {
  reset();e.S.angle=angle;e.placeDoor(15,10,"H");
  calls.length=0;translations.length=0;e.paintDoor(ctx,15,10);
  assert.deepEqual(calls.map(c=>c[0]),["hidden wall","H"]);
  assert.deepEqual(translations[1],[15*32+16,10*32+16]);
  e.placeDoor(15,10,"D");calls.length=0;e.paintDoor(ctx,15,10);
  assert.deepEqual(calls.map(c=>c[0]),["wood","wood","D"]);
}
reset();
for(let y=4;y<=16;y++) for(let x=9;x<=31;x++) e.S.map[y][x]="K";
e.S.brush="H";
assert.equal(e.arenaRect().x0,9);
assert.deepEqual(JSON.parse(JSON.stringify(e.doorSlots())),[[20,3,0],[32,10,1],[20,17,2],[8,10,3]]);
const saved=e.serializeJson(), parsed=e.parseJsonLocal(saved);
assert.equal(JSON.parse(saved).tiles[4][9],6);
assert(parsed);

const wrap=elements.canvasWrap, canvas=elements.map, stage=elements.mapStage;
wrap.clientWidth=800;wrap.clientHeight=600;
wrap.getBoundingClientRect=()=>({left:200,top:80,width:800,height:600});
for(const [prop,size,client] of [["scrollLeft","width","clientWidth"],["scrollTop","height","clientHeight"]]) {
  let v=0;
  Object.defineProperty(wrap,prop,{get(){return v;},set(n){
    v=Math.max(0,Math.min(n,Math.max(0,parseFloat(stage.style[size]||0)-wrap[client])));
  }});
}
canvas.getBoundingClientRect=()=>({
  left:200+parseFloat(canvas.style.left||0)-wrap.scrollLeft,
  top:80+parseFloat(canvas.style.top||0)-wrap.scrollTop,
  width:parseFloat(canvas.style.width||1280),height:parseFloat(canvas.style.height||736)
});
for(const point of [[220,100],[600,100],[980,100],[220,380],[600,380],[980,380],[220,660],[600,660],[980,660]]) {
  e.setZoom(.5);
  const start=e.S.zoom, before=canvas.getBoundingClientRect();
  const world=[(point[0]-before.left)/start,(point[1]-before.top)/start];
  for(const delta of [-120,-120,120,120]) {
    e.onWheel({deltaX:0,deltaY:delta,deltaMode:0,clientX:point[0],clientY:point[1],preventDefault(){}});
    const after=canvas.getBoundingClientRect();
    assert(Math.abs(after.left+world[0]*e.S.zoom-point[0])<1e-7);
    assert(Math.abs(after.top+world[1]*e.S.zoom-point[1])<1e-7);
  }
  assert(Math.abs(e.S.zoom-start)<1e-12);
}
e.onWheel({deltaX:-3,deltaY:0,deltaMode:1,clientX:600,clientY:380,preventDefault(){}});
assert(e.S.zoom>.5,"horizontal/line-mode wheel must also zoom");
reset();
e.S.mode="layout";e.S.shape="L";e.S.layoutId=null;e.S.candidateId=12;e.S.objects=[];
const locked=JSON.stringify(e.S.map);
e.putCell(15,10,"B");e.placeDoor(15,10,"D");e.removeDoorAt(15,10);
assert.equal(JSON.stringify(e.S.map),locked,"layout mode must lock terrain, door placement and erasure");
e.S.map[10][15]="B";
e.addObject("slime",15,10);e.addObject("stone",1,1);e.addObject("exit",15,10);
assert.equal(e.S.objects.length,1,"objects must be on floor; special exit excluded");
const layout=JSON.parse(e.serializeExportJson());
assert.equal(e.numberedFile("L",12),"L_012.json");
assert.equal(layout.name,"L_012");assert.equal(layout.layout_id,12);
assert.equal(layout.map,"L");assert(!layout.tiles && !layout.rotation);
e.S.mode="map";
const mapExport=JSON.parse(e.serializeExportJson());
assert(mapExport.tiles);assert.equal(mapExport.objects.length,0);
e.S.mode="layout";e.S.shape="1x1";e.S.layoutId=0;e.S.candidateId=3;e.S.objects=[];
const startLayout=JSON.parse(e.serializeExportJson());
assert.equal(e.numberedFile("1x1",0),"1x1_000.json");
assert.equal(startLayout.name,"1x1_000");assert.equal(startLayout.layout_id,0);
console.log("PASS: D/H edit+preview, hidden skins, JSON and bidirectional cursor-anchored zoom");
console.log("PASS: locked layout geometry, floor-only objects, stable numbered layout and separate map exports");
