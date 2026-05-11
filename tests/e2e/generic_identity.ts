function identity<T>(x: T): T {
    return x;
}
console.log(identity<string>("hello"));
console.log(identity<number>(7));
