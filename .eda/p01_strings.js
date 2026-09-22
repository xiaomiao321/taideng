const topIds = await eda.pcb_PrimitiveString.getAllPrimitiveId(EPCB_LayerId.TOP_SILKSCREEN);
const botIds = await eda.pcb_PrimitiveString.getAllPrimitiveId(EPCB_LayerId.BOTTOM_SILKSCREEN);
const all = await eda.pcb_PrimitiveString.getAll();

const dump = (s) => {
  const o = {};
  const proto = Object.getPrototypeOf(s);
  const names = new Set();
  let p = proto;
  while (p && p !== Object.prototype) {
    Object.getOwnPropertyNames(p).forEach((n) => names.add(n));
    p = Object.getPrototypeOf(p);
  }
  o.methods = Array.from(names);
  o.ownKeys = Object.keys(s);
  for (const k of Object.keys(s)) {
    const v = s[k];
    if (typeof v !== 'function') o[k] = v;
  }
  return o;
};

const sample = all.length ? dump(all[0]) : null;

return {
  topCount: topIds.length,
  bottomCount: botIds.length,
  allCount: all.length,
  sample,
};
