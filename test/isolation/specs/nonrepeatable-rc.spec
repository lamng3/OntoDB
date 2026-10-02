name nonrepeatable-rc
isolation rc

setup
INSERT DATA { <http://ex/a> <http://ex/type> <http://ex/Student> . }

session 1
begin: \begin
first: SELECT ?s WHERE { ?s <http://ex/type> <http://ex/Student> }
second: SELECT ?s WHERE { ?s <http://ex/type> <http://ex/Student> }
end: \commit

session 2
begin: \begin
write: INSERT DATA { <http://ex/b> <http://ex/type> <http://ex/Student> . }
end: \commit

permutation change
1.begin
1.first
2.begin
2.write
2.end
1.second
1.end

expect change
1.first contains <http://ex/a>
1.second contains <http://ex/b>
