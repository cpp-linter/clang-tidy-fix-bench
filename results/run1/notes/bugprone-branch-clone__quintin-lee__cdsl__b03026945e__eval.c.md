In `json_escape_len`, the cases for `"`/`\\` and for `\n`, `\r`, `\t`, `\b`, `\f` each did `len += 2; break;`. I merged all those labels into one branch. Behaviour is unchanged.
