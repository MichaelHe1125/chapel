/* The Computer Language Benchmarks Game
   http://benchmarksgame.alioth.debian.org

   contributed by Tom Hildebrandt, Brad Chamberlain, and Lydia Duncan
   derived from the GNU C version by Mr Ledrug
*/

use BigInteger;

config const n = 50;              // Compute n digits of pi, 50 by default
config param digitsPerLine = 10;

proc main() {

  //
  // Generate n digits, printing them in groups of digitsPerLine
  //
  for (d,i) in zip(gen_digits(n), 1..) {
    write(d);
    if i % digitsPerLine == 0 then
      writeln("\t:", i);
  }

  //
  // Pad out any trailing digits for the final line
  //
  const leftover = n%digitsPerLine;
  if (leftover) {
    for leftover..digitsPerLine do
      write(" ");
    writeln("\t:", n);
  }
}


iter gen_digits(numDigits) {
  var accum, tmp1, tmp2: bigint,
      numer, denom: bigint = 1;

  var d, k = 0;
  for i in 1..numDigits {
    do {
      do {
        k += 1;
        const k2 = 2 * k + 1;

        //
        // Compute the next term
        //
        accum += numer * 2;
        accum *= k2;
        denom *= k2;
        numer *= k;

        //
        // Continue looping until the digit is ready
        //
      } while (numer > accum);

      d = extract_digit(3);
    } while (d != extract_digit(4));       // while the 3rd digit != the 4th

    yield d;                               // once it differs, yield it

    //
    // eliminate digit d
    //
    accum -= denom * d;
    accum *= digitsPerLine;
    numer *= digitsPerLine;
  }

  //
  // Helper function to extract the nth digit
  //
  proc extract_digit(nth) {
    tmp2 = numer * nth;
    tmp2 += accum;
    tmp2 /= denom;
    
    return tmp2.get_si();
  }
}


