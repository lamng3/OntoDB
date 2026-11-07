name si-write-skew
isolation si

setup
INSERT DATA { <http://ex/x> <http://ex/on> <http://ex/call> . <http://ex/y> <http://ex/on> <http://ex/call> . }

session 1
begin: \begin
read: SELECT ?s WHERE { ?s <http://ex/on> <http://ex/call> }
del: DELETE DATA { <http://ex/x> <http://ex/on> <http://ex/call> . }
end: \commit

session 2
begin: \begin
read: SELECT ?s WHERE { ?s <http://ex/on> <http://ex/call> }
del: DELETE DATA { <http://ex/y> <http://ex/on> <http://ex/call> . }
end: \commit
after: SELECT ?s WHERE { ?s <http://ex/on> <http://ex/call> }

permutation allowed
1.begin
2.begin
1.read
2.read
1.del
2.del
1.end
2.end
2.after

expect allowed
1.read contains <http://ex/y>
2.read contains <http://ex/x>
2.after absent <http://ex/x>
2.after absent <http://ex/y>
