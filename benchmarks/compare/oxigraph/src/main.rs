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
// path: SELECT DISTINCT ?x WHERE { ?c rdfs:subClassOf* <C> . ?x a ?c } (path first: Jena does not reorder it).

use oxigraph::io::{RdfFormat, RdfParser};
use oxigraph::model::NamedNode;
use oxigraph::sparql::{PreparedSparqlQuery, QueryResults, SparqlEvaluator};
use oxigraph::store::Store;
use std::fs::{self, File};
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::{Duration, Instant};
use oxigraph::model::{GraphNameRef, NamedNodeRef, QuadRef};

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


// Threads against one store: `lookup` readers, `insert` writers, or one writer beside readers.
// Queries come from the (s,.,.) and (s,p,.) shapes of the query list; a writer inserts one
// triple per call (each call is its own transaction) with a scattered new subject.
fn concurrency(store: &Store, queries: &str, seconds: f64) {
    let mut prepared = Vec::new();
    for line in BufReader::new(File::open(queries).unwrap()).lines() {
        let line = line.unwrap();
        let cols: Vec<&str> = line.split('\t').collect();
        if cols.len() < 2 || !(cols[0].starts_with("s--") || cols[0].starts_with("sp-")) {
            continue;
        }
        prepared.push(SparqlEvaluator::new().parse_query(cols[1]).unwrap());
    }
    let next = AtomicU64::new(0);
    let run = |readers: usize, writers: usize| -> (f64, f64) {
        let stop = AtomicBool::new(false);
        let reads = AtomicU64::new(0);
        let inserts = AtomicU64::new(0);
        let start = Instant::now();
        std::thread::scope(|scope| {
            for r in 0..readers {
                let (stop, reads, prepared) = (&stop, &reads, &prepared);
                scope.spawn(move || {
                    let mut at = r * 7;
                    let mut mine = 0u64;
                    while !stop.load(Ordering::Relaxed) {
                        let q = prepared[at % prepared.len()].clone();
                        if let QueryResults::Solutions(solutions) = q.on_store(store).execute().unwrap() {
                            for solution in solutions {
                                let solution = solution.unwrap();
                                for (_, term) in solution.iter() {
                                    std::hint::black_box(term.to_string().len());
                                }
                            }
                        }
                        at += 1;
                        mine += 1;
                    }
                    reads.fetch_add(mine, Ordering::Relaxed);
                });
            }
            for _ in 0..writers {
                let (stop, inserts, next) = (&stop, &inserts, &next);
                scope.spawn(move || {
                    let p = NamedNodeRef::new("http://example.org/new/p").unwrap();
                    let o = NamedNodeRef::new("http://example.org/new/o").unwrap();
                    let mut mine = 0u64;
                    while !stop.load(Ordering::Relaxed) {
                        let id = next.fetch_add(1, Ordering::Relaxed).wrapping_mul(2654435761) & ((1u64 << 26) - 1);
                        let s = NamedNode::new(format!("http://example.org/new/s{}", id)).unwrap();
                        store.insert(QuadRef::new(&s, p, o, GraphNameRef::DefaultGraph)).unwrap();
                        mine += 1;
                    }
                    inserts.fetch_add(mine, Ordering::Relaxed);
                });
            }
            std::thread::sleep(Duration::from_secs_f64(seconds));
            stop.store(true, Ordering::Relaxed);
        });
        let elapsed = start.elapsed().as_secs_f64();
        (reads.load(Ordering::Relaxed) as f64 / elapsed, inserts.load(Ordering::Relaxed) as f64 / elapsed)
    };
    for t in [1usize, 2, 4, 8] {
        let (r, _) = run(t, 0);
        println!("read\tlookup\t{}\t{:.0}\t0", t, r);
    }
    for t in [1usize, 2, 4, 8] {
        let (_, w) = run(0, t);
        println!("insert\t-\t{}\t0\t{:.0}", t, w);
    }
    for t in [1usize, 2, 4, 8] {
        let (r, w) = run(t, 1);
        println!("mixed\tlookup\t{}\t{:.0}\t{:.0}", t, r, w);
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mode = &args[1];
    let work = PathBuf::from(&args[2]);
    if args[3] == "conc" {
        let work = PathBuf::from(&args[2]);
        let (store, _dir) = if args[1] == "memory" {
            (Store::new().unwrap(), None)
        } else {
            let dir = work.join("rocksdb");
            let _ = fs::remove_dir_all(&dir);
            fs::create_dir_all(&work).unwrap();
            (Store::open(&dir).unwrap(), Some(dir))
        };
        {
            let mut loader = store.bulk_loader();
            for f in &args[6..] {
                let path = fs::canonicalize(f).unwrap();
                let parser = RdfParser::from_format(RdfFormat::RdfXml)
                    .with_base_iri(format!("file://{}", path.display()))
                    .unwrap();
                loader.load_from_reader(parser, BufReader::new(File::open(&path).unwrap())).unwrap();
            }
            loader.commit().unwrap();
        }
        concurrency(&store, &args[4], args[5].parse().unwrap());
        return;
    }
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
                "PREFIX rdfs: <http://www.w3.org/2000/01/rdf-schema#> SELECT DISTINCT ?x WHERE {{ ?c rdfs:subClassOf* {} . ?x a ?c }}",
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
