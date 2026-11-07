name si-own-writes
isolation si

setup
INSERT DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }

session 1
begin: \begin
ins: INSERT DATA { <http://ex/b> <http://ex/p> <http://ex/o> . }
del: DELETE DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }
read: SELECT ?s WHERE { ?s <http://ex/p> <http://ex/o> }
end: \abort
after: SELECT ?s WHERE { ?s <http://ex/p> <http://ex/o> }

permutation own
1.begin
1.ins
1.del
1.read
1.end
1.after

expect own
1.read contains <http://ex/b>
1.read absent <http://ex/a>
1.after contains <http://ex/a>
1.after absent <http://ex/b>
