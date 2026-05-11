type Cfg = { name: string; port: number };
function format(c: Cfg): string {
    return `${c.name}:${c.port}`;
}
console.log(format({ name: "server", port: 8080 }));
