const comps = await eda.pcb_PrimitiveComponent.getAll();
const u7 = comps.find((c) => c.getState_Designator() === 'U7');
const rawPads = u7.getState_Pads();
const first = rawPads[0];
const own = {};
for (const k of Object.keys(first)) own[k] = first[k];
const protoNames = [];
let p = Object.getPrototypeOf(first);
while (p && p !== Object.prototype) {
  protoNames.push(...Object.getOwnPropertyNames(p));
  p = Object.getPrototypeOf(p);
}
const padAll = await eda.pcb_PrimitivePad.getAll();
const firstPad = padAll[0];
const firstPadDump = {};
for (const k of Object.keys(firstPad)) firstPadDump[k] = firstPad[k];
return {
  rawPadCount: rawPads.length,
  rawPadIsString: typeof first,
  rawPadOwnKeys: Object.keys(first),
  rawPadOwn: own,
  rawPadProto: protoNames,
  padAllCount: padAll.length,
  firstPadOwnKeys: Object.keys(firstPad),
  firstPadOwn: firstPadDump,
};
