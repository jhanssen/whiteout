const v = "42" as string;
console.log(v);
const obj = { a: 1, b: 2 } satisfies { a: number; b: number };
console.log(obj.a + obj.b);
