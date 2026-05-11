interface Greeter {
    greet(name: string): string;
}
class Hello implements Greeter {
    greet(name: string): string {
        return `hi ${name}`;
    }
}
console.log(new Hello().greet("world"));
