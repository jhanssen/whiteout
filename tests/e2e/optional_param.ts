function fmt(prefix: string, name?: string): string {
    return name ? `${prefix} ${name}` : prefix;
}
console.log(fmt("hi", "there"));
console.log(fmt("yo"));
