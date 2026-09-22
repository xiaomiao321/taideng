const names = ['pcb_Document', 'pcb_Layer', 'pcb_Net', 'pcb_Primitive', 'pcb_PrimitivePad',
  'pcb_PrimitiveString', 'pcb_PrimitiveComponent', 'pcb_PrimitiveLine', 'pcb_PrimitiveVia',
  'pcb_PrimitivePour', 'pcb_PrimitiveFill', 'pcb_PrimitiveRegion', 'pcb_PrimitivePoured',
  'pcb_SelectControl', 'pcb_Drc', 'pcb_ManufactureData', 'sys_Unit', 'sys_FontManager',
  'sys_Setting', 'sys_Dialog', 'sys_Message', 'sys_Storage'];

const methods = (obj) => {
  const set = new Set();
  let p = Object.getPrototypeOf(obj);
  while (p && p !== Object.prototype) {
    Object.getOwnPropertyNames(p).forEach((n) => set.add(n));
    p = Object.getPrototypeOf(p);
  }
  Object.getOwnPropertyNames(obj).forEach((n) => set.add(n));
  return Array.from(set).sort();
};

const out = {};
for (const n of names) {
  const o = eda[n];
  out[n] = o ? methods(o) : 'MISSING';
}
return out;
