// Oxigraph's side of the comparison with OntoDB (benchmarks/compare).
//
//   oxigraph-bench <rocksdb|memory> <work dir> <queries.tsv> <runs> <rdf file>...
//   oxigraph-bench <rocksdb|memory> <work dir> types <classes.tsv> <runs> <rdf file>...
//
// Loads the files with the bulk loader, then runs every query of <queries.tsv> (name TAB one-line
// SPARQL) once to warm and `runs` times to time, printing name, rows, median microseconds. Every
// term of every solution is turned into its string, the way OntoDB decodes its rows. The first
// line reports the load, in the same format as ontodb_bench_compare.
//
// `types` reads classes.tsv from `ontodb_bench_semantic types` (bucket, class IRI, expected
// answers, OntoDB's microseconds) and asks for the instances of each class through the subclass
// path: SELECT DISTINCT ?x WHERE { ?x a ?c . ?c rdfs:subClassOf* <C> }.

use oxigraph::io::{RdfFormat, RdfParser};
use oxigraph::sparql::{PreparedSparqlQuery, QueryResults, SparqlEvaluator};
use oxigraph::store::Store;
use std::fs::{self, File};
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::time::Instant;

fn dir_size(path: &Path) -> u64 {
    let mut total = 0;
    if let Ok(entries) = fs::read_dir(path) {
        for entry in entries.flatten() {
            let meta = entry.metadata().unwrap();
            total += if meta.is_dir() { dir_size(&entry.path()) } else { meta.len() };
        }
    }
    total
}

fn rss_mib() -> f64 {
    let out = std::process::Command::new("ps")
        .args(["-o", "rss=", "-p", &std::process::id().to_string()])
        .output()
        .unwrap();
    String::from_utf8_lossy(&out.stdout).trim().parse::<f64>().unwrap_or(0.0) / 1024.0
}

// The query is parsed once, outside the timer, as OntoDB's side binds once; planning and
// evaluation, which OntoDB also does inside its timer, are timed.
fn run_once(store: &Store, query: &PreparedSparqlQuery) -> (usize, f64) {
    let started = Instant::now();
    let mut rows = 0;
    let mut bytes = 0;
    if let QueryResults::Solutions(solutions) = query.clone().on_store(store).execute().unwrap() {
        for solution in solutions {
            let solution = solution.unwrap();
            for (_, term) in solution.iter() {
                bytes += term.to_string().len();
            }
            rows += 1;
        }
    }
    std::hint::black_box(bytes);
    (rows, started.elapsed().as_secs_f64() * 1e6)
}

fn timed(store: &Store, text: &str, runs: usize) -> (usize, f64) {
    let query = SparqlEvaluator::new().parse_query(text).unwrap();
    let (rows, first) = run_once(store, &query); // warms the caches
    let n = if first > 2e6 { 1 } else { runs };
    let mut times: Vec<f64> = (0..n).map(|_| run_once(store, &query).1).collect();
    times.sort_by(|a, b| a.partial_cmp(b).unwrap());
    (rows, times[times.len() / 2])
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mode = &args[1];
    let work = PathBuf::from(&args[2]);
    let types = args[3] == "types";
    let queries = &args[if types { 4 } else { 3 }];
    let runs: usize = args[if types { 5 } else { 4 }].parse().unwrap();
    let files = &args[if types { 6 } else { 5 }..];

    let (store, dir) = if mode == "memory" {
        (Store::new().unwrap(), None)
    } else {
        let dir = work.join("rocksdb");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&work).unwrap();
        (Store::open(&dir).unwrap(), Some(dir))
    };
    let started = Instant::now();
    {
        let mut loader = store.bulk_loader();
        for f in files {
            let path = fs::canonicalize(f).unwrap();
            let format = if f.ends_with(".nt") { RdfFormat::NTriples } else { RdfFormat::RdfXml };
            let parser = RdfParser::from_format(format)
                .with_base_iri(format!("file://{}", path.display()))
                .unwrap();
            loader.load_from_reader(parser, BufReader::new(File::open(&path).unwrap())).unwrap();
        }
        loader.commit().unwrap();
    }
    store.flush().unwrap();
    let load_s = started.elapsed().as_secs_f64();
    let triples = store.len().unwrap();
    let file_mib = dir.as_ref().map_or(0.0, |d| dir_size(d) as f64 / (1024.0 * 1024.0));
    println!(
        "#load\ttriples={}\tload_s={:.2}\tstatistics_s=0\tfile_mib={:.1}\tpeak_rss_mib={:.0}",
        triples, load_s, file_mib, rss_mib()
    );

    for line in BufReader::new(File::open(queries).unwrap()).lines() {
        let line = line.unwrap();
        let cols: Vec<&str> = line.split('\t').collect();
        if cols.len() < 2 {
            continue;
        }
        if types {
            let class = cols[1];
            let text = format!(
                "PREFIX rdfs: <http://www.w3.org/2000/01/rdf-schema#> SELECT DISTINCT ?x WHERE {{ ?x a ?c . ?c rdfs:subClassOf* {} }}",
                class
            );
            let (rows, us) = timed(&store, &text, runs);
            println!("{}\t{}\t{}\t{}\t{:.1}", cols[0], class, cols[2], rows, us);
        } else {
            let (rows, us) = timed(&store, cols[1], runs);
            println!("{}\t{}\t{:.1}", cols[0], rows, us);
        }
    }
}
