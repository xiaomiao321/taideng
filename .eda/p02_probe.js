const g = typeof globalThis !== 'undefined' ? globalThis : this;
const globals = Object.getOwnPropertyNames(g).filter((n) => /EPCB|EDMT|ESCH|ELIB|eda/i.test(n));
const edaKeys = Object.keys(eda).sort();
const edaOwn = Object.getOwnPropertyNames(eda).sort();
let protoChain = [];
let p = Object.getPrototypeOf(eda);
while (p && p !== Object.prototype) {
  protoChain = protoChain.concat(Object.getOwnPropertyNames(p));
  p = Object.getPrototypeOf(p);
}
const enumLike = {};
for (const k of edaOwn) {
  let v;
  try { v = eda[k]; } catch (e) { continue; }
  if (v && typeof v === 'object' && !Array.isArray(v) && /^E[A-Z]+_/.test(k)) {
    enumLike[k] = v;
  }
}
return {
  globalsWithEdaName: globals,
  typeofEda: typeof eda,
  edaKeysCount: edaKeys.length,
  edaKeys: edaOwn,
  protoChain,
  enumLikeKeys: Object.keys(enumLike),
  EPCB_LayerId_viaEda: eda.EPCB_LayerId ? eda.EPCB_LayerId.TOP_SILKSCREEN : null,
};
