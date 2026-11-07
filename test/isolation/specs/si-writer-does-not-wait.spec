name si-writer-does-not-wait
isolation si

setup
INSERT DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }

session 1
begin: \begin
read: SELECT ?s WHERE { ?s <http://ex/p> <http://ex/o> }
again: SELECT ?s WHERE { ?s <http://ex/p> <http://ex/o> }
end: \commit

session 2
del: DELETE DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }

permutation reader-first
1.begin
1.read
2.del
1.again
1.end

expect reader-first
1.read contains <http://ex/a>
1.again contains <http://ex/a>
