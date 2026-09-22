const num = (v) => (typeof v === 'number' ? Math.round(v * 1000) / 1000 : v);

const comps = await eda.pcb_PrimitiveComponent.getAll();
const pads = await eda.pcb_PrimitivePad.getAll();
const padById = {};
for (const p of pads) padById[p.getState_PrimitiveId()] = p;

const outlineIds = await eda.pcb_PrimitivePolyline.getAllPrimitiveId();
const outlinePts = [];
for (const id of outlineIds) {
  const line = eda.pcb_Primitive.getPrimitiveBoardLine(id);
  if (line && typeof line.getSourceStrictComplex === 'function') {
    const src = line.getSourceStrictComplex();
    outlinePts.push({ id, src });
  }
}

const compList = [];
for (const c of comps) {
  const cid = c.getState_PrimitiveId();
  const rawPads = c.getState_Pads() || [];
  const padIds = rawPads.map((p) => {
    if (typeof p === 'string') return p;
    if (p && typeof p.getState_PrimitiveId === 'function') return p.getState_PrimitiveId();
    if (p && p.primitiveId) return p.primitiveId;
    return String(p);
  });
  const padInfo = [];
  for (const pid of padIds) {
    const p = padById[pid];
    if (!p) { padInfo.push({ id: pid, missing: true }); continue; }
    padInfo.push({
      id: pid,
      num: p.getState_PadNumber(),
      net: p.getState_Net(),
      layer: p.getState_Layer(),
      x: num(p.getState_X()),
      y: num(p.getState_Y()),
      rot: num(p.getState_Rotation()),
      pad: p.getState_Pad(),
      hole: p.getState_Hole(),
      padType: p.getState_PadType(),
      metallization: p.getState_Metallization(),
    });
  }
  compList.push({
    id: cid,
    designator: c.getState_Designator(),
    name: c.getState_Name(),
    comp: c.getState_Component(),
    footprint: c.getState_Footprint(),
    layer: c.getState_Layer(),
    x: num(c.getState_X()),
    y: num(c.getState_Y()),
    rot: num(c.getState_Rotation()),
    other: c.getState_OtherProperty(),
    pads: padInfo,
  });
}

const netMap = {};
for (const c of compList) {
  for (const p of c.pads) {
    const n = p.net === undefined || p.net === null || p.net === '' ? '(none)' : String(p.net);
    if (!netMap[n]) netMap[n] = [];
    netMap[n].push(c.designator + '.' + p.num);
  }
}
const nets = Object.keys(netMap).sort().map((n) => ({ net: n, pins: netMap[n], count: netMap[n].length }));

const samplePad = pads.length ? pads[0] : null;
return {
  outlinePts,
  compCount: compList.length,
  comps: compList,
  netCount: nets.length,
  nets,
};
