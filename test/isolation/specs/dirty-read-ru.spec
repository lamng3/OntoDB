name dirty-read-ru
isolation ru

setup
INSERT DATA { <http://ex/a> <http://ex/type> <http://ex/Student> . }

session 1
begin: \begin
write: INSERT DATA { <http://ex/b> <http://ex/type> <http://ex/Student> . }
end: \commit

session 2
begin: \begin
read: SELECT ?s WHERE { ?s <http://ex/type> <http://ex/Student> }
end: \commit

permutation sees
1.begin
2.begin
1.write
2.read
1.end
2.end

expect sees
2.read contains <http://ex/b>
