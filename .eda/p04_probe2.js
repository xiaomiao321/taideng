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

const padIds = await eda.pcb_PrimitivePad.getAllPrimitiveId();
const pads = padIds.length ? await eda.pcb_PrimitivePad.get(padIds.slice(0, 1)) : [];
const comps = await eda.pcb_PrimitiveComponent.getAll();
const lines = await eda.pcb_PrimitiveLine.getAll();
const vias = await eda.pcb_PrimitiveVia.getAll();
const regions = await eda.pcb_PrimitiveRegion.getAll();
const fills = await eda.pcb_PrimitiveFill.getAll();
const pours = await eda.pcb_PrimitivePour.getAll();

const layerHistogram = (arr, getLayer) => {
  const h = {};
  for (const o of arr) {
    let l = null;
    try { l = getLayer(o); } catch (e) { l = 'ERR'; }
    h[l] = (h[l] || 0) + 1;
  }
  return h;
};

const boardOutlines = lines.filter((l) => l.getState_Layer() === 11);
const boardBBox = boardOutlines.length
  ? await eda.pcb_Primitive.getPrimitivesBBox(boardOutlines.map((o) => o.getState_PrimitiveId()))
  : null;

return {
  padMethodNames: pads[0] ? methods(pads[0]) : 'no pads',
  compMethodNames: comps[0] ? methods(comps[0]) : 'no comps',
  compCount: comps.length,
  padCount: pads.length === 0 ? 0 : padIds.length,
  lineCount: lines.length,
  viaCount: vias.length,
  regionCount: regions.length,
  fillCount: fills.length,
  pourCount: pours.length,
  lineLayerHistogram: layerHistogram(lines, (o) => o.getState_Layer()),
  regionLayerHistogram: layerHistogram(regions, (o) => o.getState_Layer()),
  fillLayerHistogram: layerHistogram(fills, (o) => o.getState_Layer()),
  pourLayerHistogram: layerHistogram(pours, (o) => o.getState_Layer()),
  boardOutlineCount: boardOutlines.length,
  boardBBox,
};
