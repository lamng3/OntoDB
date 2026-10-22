// node web/smoke.mjs: runs the WebAssembly build the way the playground does.
import fs from "node:fs";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const OntoDB = require("./out/ontodb_wasm.js");
const m = await OntoDB();
const exec = m.cwrap("ontodb_exec", "string", ["string"]);
const pizza = process.argv[2] || new URL("../data/pizza.ttl", import.meta.url).pathname;
m.FS.writeFile("/pizza.ttl", fs.readFileSync(pizza));
const run = (line) => { const out = exec(line); console.log("OntoDB> " + line.split("\n")[0] + "\n" + out + "\n"); return out; };
run("\\load /pizza.ttl");
run("\\stats");
run("PREFIX pizza: <http://www.co-ode.org/ontologies/pizza/pizza.owl#> PREFIX rdfs: <http://www.w3.org/2000/01/rdf-schema#> SELECT ?p ?l WHERE { ?p rdfs:subClassOf pizza:NamedPizza . ?p rdfs:label ?l }");
run("\\backend indexed");
run("PREFIX pizza: <http://www.co-ode.org/ontologies/pizza/pizza.owl#> SELECT ?t WHERE { pizza:MargheritaPizza pizza:hasTopping ?t }");
run("\\explain analyze PREFIX pizza: <http://www.co-ode.org/ontologies/pizza/pizza.owl#> SELECT ?t WHERE { pizza:MargheritaPizza pizza:hasTopping ?t }");
run("SELECT ?x WHERE { ?x ");
