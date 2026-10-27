// Minimal first-light script: if this prints, the interpreter runs.
print("hello from jsc on " + (typeof navigator === "undefined" ? "a shell" : navigator.userAgent));
print("1+1 = " + (1 + 1) + ", Math.PI = " + Math.PI + ", Date = " + new Date(0).toISOString());
