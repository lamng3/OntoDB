// Apache Jena TDB2's side of the comparison with OntoDB (benchmarks/compare).
//
//   java Bench <work dir> <queries.tsv> <runs> <rdf file>...
//   java Bench <work dir> types <classes.tsv> <runs> <rdf file>...
//
// Loads the files with TDB2's own bulk loader (tdb2.tdbloader), then runs every query of
// <queries.tsv> (name TAB one-line SPARQL) three times to warm the JIT and `runs` times to time,
// printing name, rows, median microseconds. Every node of every solution is turned into its
// string. The first line reports the load in the format of ontodb_bench_compare. `types` asks for
// the instances of each class of classes.tsv through the subclass path.

import java.io.IOException;
import java.nio.file.*;
import java.util.*;
import java.util.stream.Stream;
import org.apache.jena.query.*;
import org.apache.jena.rdf.model.RDFNode;
import org.apache.jena.sparql.util.FmtUtils;
import org.apache.jena.system.Txn;
import org.apache.jena.tdb2.TDB2Factory;

public class Bench {
    static long dirSize(Path p) throws IOException {
        try (Stream<Path> s = Files.walk(p)) {
            return s.filter(Files::isRegularFile).mapToLong(f -> f.toFile().length()).sum();
        }
    }

    static double rssMib() throws Exception {
        Process pr = new ProcessBuilder("ps", "-o", "rss=", "-p", String.valueOf(ProcessHandle.current().pid())).start();
        String out = new String(pr.getInputStream().readAllBytes()).trim();
        return Double.parseDouble(out) / 1024.0;
    }

    static long bytes;

    static int runOnce(Dataset ds, Query query, double[] micros, int slot) {
        long start = System.nanoTime();
        int[] rows = {0};
        Txn.executeRead(ds, () -> {
            try (QueryExecution qe = QueryExecution.create().query(query).dataset(ds)
                     .timeout(60, java.util.concurrent.TimeUnit.SECONDS).build()) { // a minute is a timeout
                ResultSet rs = qe.execSelect();
                while (rs.hasNext()) {
                    QuerySolution sol = rs.next();
                    for (Iterator<String> v = sol.varNames(); v.hasNext(); ) {
                        RDFNode n = sol.get(v.next());
                        if (n != null) bytes += FmtUtils.stringForNode(n.asNode()).length();
                    }
                    rows[0]++;
                }
            }
        });
        micros[slot] = (System.nanoTime() - start) / 1e3;
        return rows[0];
    }

    static String timed(Dataset ds, String text, int runs, int[] rowsOut) {
        try {
            return timedRuns(ds, text, runs, rowsOut);
        } catch (org.apache.jena.query.QueryCancelledException e) {
            rowsOut[0] = -1;
            return "timeout";
        }
    }

    static String timedRuns(Dataset ds, String text, int runs, int[] rowsOut) {
        Query query = QueryFactory.create(text);
        double[] t = new double[1];
        for (int i = 0; i < 3; i++) rowsOut[0] = runOnce(ds, query, t, 0); // warms the JIT and the caches
        int n = t[0] > 2e6 ? 1 : runs;
        double[] times = new double[n];
        for (int i = 0; i < n; i++) runOnce(ds, query, times, i);
        Arrays.sort(times);
        return String.format("%.1f", times[n / 2]);
    }

    public static void main(String[] a) throws Exception {
        Path work = Paths.get(a[0]);
        boolean types = a[1].equals("types");
        String queries = a[types ? 2 : 1];
        int runs = Integer.parseInt(a[types ? 3 : 2]);
        List<String> files = Arrays.asList(a).subList(types ? 4 : 3, a.length);
        Path dir = work.resolve("tdb2");
        if (Files.exists(dir)) try (Stream<Path> s = Files.walk(dir)) { s.sorted(Comparator.reverseOrder()).forEach(p -> p.toFile().delete()); }
        Files.createDirectories(dir);
        long t0 = System.nanoTime();
        List<String> args = new ArrayList<>(List.of("--loc", dir.toString()));
        args.addAll(files);
        tdb2.tdbloader.main(args.toArray(new String[0]));
        double loadS = (System.nanoTime() - t0) / 1e9;
        Dataset ds = TDB2Factory.connectDataset(dir.toString());
        long[] triples = {0};
        Txn.executeRead(ds, () -> triples[0] = ds.getDefaultModel().size());
        System.out.printf("#load\ttriples=%d\tload_s=%.2f\tstatistics_s=0\tfile_mib=%.1f\tpeak_rss_mib=%.0f%n",
            triples[0], loadS, dirSize(dir) / 1048576.0, rssMib());
        for (String line : Files.readAllLines(Paths.get(queries))) {
            String[] c = line.split("\t");
            if (c.length < 2) continue;
            int[] rows = {0};
            if (types) {
                String text = "PREFIX rdfs: <http://www.w3.org/2000/01/rdf-schema#> SELECT DISTINCT ?x WHERE { ?c rdfs:subClassOf* " + c[1] + " . ?x a ?c }";
                String us = timed(ds, text, runs, rows);
                System.out.printf("%s\t%s\t%s\t%d\t%s%n", c[0], c[1], c[2], rows[0], us);
            } else {
                String us = timed(ds, c[1], runs, rows);
                System.out.printf("%s\t%d\t%s%n", c[0], rows[0], us);
            }
        }
        if (bytes == 42) System.out.print("");
        System.exit(0);
    }
}
