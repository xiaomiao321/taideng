const safe = async (fn, fallback) => { try { return await fn(); } catch (e) { return fallback === undefined ? 'ERR:' + e.message : fallback; } };

const layers = await safe(() => eda.pcb_Layer.getAllLayers(), []);
const layerInfo = Array.isArray(layers) ? layers.map((l) => ({ id: l.id, name: l.name, type: l.type, visible: l.layerStatus, locked: l.locked })) : layers;

const types = ['pcb_PrimitiveLine', 'pcb_PrimitiveArc', 'pcb_PrimitiveFill', 'pcb_PrimitiveRegion',
  'pcb_PrimitivePour', 'pcb_PrimitivePoured', 'pcb_PrimitivePolyline', 'pcb_PrimitiveDimension',
  'pcb_PrimitiveImage', 'pcb_PrimitiveAttribute', 'pcb_PrimitivePad', 'pcb_PrimitiveVia', 'pcb_PrimitiveString'];

const counts = {};
const perLayer = {};
for (const t of types) {
  const mod = eda[t];
  if (!mod || typeof mod.getAll !== 'function') { counts[t] = 'NO_getAll'; continue; }
  const arr = await safe(() => mod.getAll(), []);
  if (!Array.isArray(arr)) { counts[t] = String(arr); continue; }
  counts[t] = arr.length;
  const h = {};
  for (const o of arr) {
    let l = null;
    try { l = typeof o.getState_Layer === 'function' ? o.getState_Layer() : null; } catch (e) { l = 'ERR'; }
    h[l] = (h[l] || 0) + 1;
  }
  perLayer[t] = h;
}

const flat = {};
for (const [t, h] of Object.entries(perLayer)) {
  for (const [l, n] of Object.entries(h)) {
    flat[l] = flat[l] || {};
    flat[l][t] = n;
  }
}

const copperLayers = await safe(() => eda.pcb_Layer.getTheNumberOfCopperLayers(), null);
const stacking = await safe(() => eda.pcb_Layer.getCurrentPhysicalStackingConfigurationName(), null);

return { layerInfo, counts, perLayerFlat: flat, copperLayers, stacking };
