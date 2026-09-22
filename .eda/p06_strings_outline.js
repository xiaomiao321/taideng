const strIds = await eda.pcb_PrimitiveString.getAllPrimitiveId();
const strings = await eda.pcb_PrimitiveString.get(strIds);
const strDump = [];
for (const s of (Array.isArray(strings) ? strings : [strings])) {
  strDump.push({
    id: s.getState_PrimitiveId(),
    layer: s.getState_Layer(),
    x: s.getState_X(),
    y: s.getState_Y(),
    text: s.getState_Text(),
    fontFamily: s.getState_FontFamily(),
    fontSize: s.getState_FontSize(),
    lineWidth: s.getState_LineWidth(),
    alignMode: s.getState_AlignMode(),
    rotation: s.getState_Rotation(),
    reverse: s.getState_Reverse(),
    expansion: s.getState_Expansion(),
    mirror: s.getState_Mirror(),
    lock: s.getState_PrimitiveLock(),
    bbox: await eda.pcb_Primitive.getPrimitivesBBox([s.getState_PrimitiveId()]),
  });
}

const polyIds = await eda.pcb_PrimitivePolyline.getAllPrimitiveId();
const polys = await eda.pcb_PrimitivePolyline.get(polyIds);
const polyDump = (Array.isArray(polys) ? polys : [polys]).map((p) => ({
  id: p.getState_PrimitiveId(),
  layer: p.getState_Layer(),
  bbox: null,
}));
const polyBBox = await eda.pcb_Primitive.getPrimitivesBBox(polyIds);

const attrs = await eda.pcb_PrimitiveAttribute.getAll();
const attrSummary = {};
for (const a of attrs) {
  const key = String(a.getState_Key());
  attrSummary[key] = attrSummary[key] || { total: 0, onTop: 0, onBottom: 0, reverseTrue: 0, visibleKey: 0, visibleValue: 0, fonts: {}, sizes: {} };
  const s = attrSummary[key];
  s.total += 1;
  const l = a.getState_Layer();
  if (l === 3) s.onTop += 1;
  if (l === 4) s.onBottom += 1;
  if (a.getState_Reverse()) s.reverseTrue += 1;
  if (a.getState_KeyVisible()) s.visibleKey += 1;
  if (a.getState_ValueVisible()) s.visibleValue += 1;
  const f = String(a.getState_FontFamily());
  s.fonts[f] = (s.fonts[f] || 0) + 1;
  const sz = String(a.getState_FontSize());
  s.sizes[sz] = (s.sizes[sz] || 0) + 1;
}

const attrsVisible = attrs
  .filter((a) => (a.getState_KeyVisible() || a.getState_ValueVisible()))
  .map((a) => ({
    id: a.getState_PrimitiveId(),
    parent: a.getState_ParentPrimitiveId(),
    layer: a.getState_Layer(),
    key: a.getState_Key(),
    value: a.getState_Value(),
    keyVisible: a.getState_KeyVisible(),
    valueVisible: a.getState_ValueVisible(),
    x: a.getState_X(),
    y: a.getState_Y(),
    fontSize: a.getState_FontSize(),
    fontFamily: a.getState_FontFamily(),
    rotation: a.getState_Rotation(),
    reverse: a.getState_Reverse(),
    expansion: a.getState_Expansion(),
    mirror: a.getState_Mirror(),
  }));

return {
  strings: strDump,
  polylineIds: polyIds,
  polylineBBox: polyBBox,
  polyDump,
  attrKeySummary: attrSummary,
  attrTotal: attrs.length,
  visibleAttrCount: attrsVisible.length,
  visibleAttrs: attrsVisible.slice(0, 40),
};
