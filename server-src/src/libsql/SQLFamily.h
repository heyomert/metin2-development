#pragma once

#include <cstddef>

// A statement's family label WITHOUT any value from it, for logs and the failure ledger
// (docs/engineering/db-step2-asyncsql-fix.md, section 7b): "<verb>" or "<verb>.<table>", lower case, [a-z0-9_.] only.
// The table is the identifier after FROM / INTO, or the target of UPDATE. Quoted strings ('...', "...") are skipped
// with their backslash and doubled-quote escapes, so nothing inside a literal can reach the label; when in doubt the
// table is left out ("unknown" if even the verb is not recognised).
void SQLFamily(const char* sql, char* out, size_t outSize);
