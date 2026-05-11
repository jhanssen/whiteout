const m: Map<string, Array<number>> = new Map();
m.set("a", [1, 2, 3]);
console.log(m.get("a")!.reduce((s, v) => s + v, 0));
