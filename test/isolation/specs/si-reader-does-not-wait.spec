name si-reader-does-not-wait
isolation si

setup
INSERT DATA { <http://ex/a> <http://ex/type> <http://ex/Student> . }

session 1
begin: \begin
ins: INSERT DATA { <http://ex/b> <http://ex/type> <http://ex/Student> . }
end: \commit

session 2
begin: \begin
read: SELECT ?s WHERE { ?s <http://ex/type> <http://ex/Student> }
again: SELECT ?s WHERE { ?s <http://ex/type> <http://ex/Student> }
end: \commit
next: SELECT ?s WHERE { ?s <http://ex/type> <http://ex/Student> }

permutation writer-first
1.begin
1.ins
2.begin
2.read
1.end
2.again
2.end
2.next

expect writer-first
2.read contains <http://ex/a>
2.read absent <http://ex/b>
2.again absent <http://ex/b>
2.next contains <http://ex/b>
