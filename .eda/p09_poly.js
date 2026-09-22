const ids = await eda.pcb_PrimitivePolyline.getAllPrimitiveId();
const all = await eda.pcb_PrimitivePolyline.get(ids);
const arr = Array.isArray(all) ? all : [all];
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
const o = arr[0];
const own = {};
for (const k of Object.keys(o)) {
  const v = o[k];
  own[k] = typeof v === 'function' ? '[fn]' : v;
}
return {
  ids,
  methods: methods(o),
  own,
  bbox: await eda.pcb_Primitive.getPrimitivesBBox(ids),
};
