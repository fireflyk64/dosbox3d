// The sales' scripts (wcnet_priv_convs.cpp).  "%s" in a text is the ship's name.  A piece of speech: the game's
// conversation, its line (counted from 0, lines only), from and to in milliseconds (to 0: the line's end), and the
// quiet after it.  What each piece SAYS is in the comment beside it (scripts/privships/convwords.py prints a
// conversation's spoken words and their times).

#define T(mood, text) kLine, kTerrell, mood, text
#define P(mood, text) kLine, kPlayer, mood, text
#define D(text) kLine, kDealer, "normal", text
#define NONE { 0, 0, 0, 0, 0 }

// ---- The Stiletto, from Admiral Terrell (after his credits) ---------------------------------------------------------
// The story: the Admiral, still recruiting, sweetens his offer with a Navy light fighter; the base's ship dealer does
// the selling (his are the game's only spoken words about ships and money); the privateer takes the ship, not the
// commission.

const Line kStilettoOffer[] = {
    { T("normal", "And that's not all, $NM."), { { "CREDITS", 42, 0, 1100, 0 }, NONE } },                        // "And that's not all."
    { P("normal", "Then what exactly do you want from me?"), { { "S7MBC1", 6, 0, 0, 0 }, NONE } },
    { T("normal", "You're an insubordinate smart ass...and one of the best pilots I've ever seen."), { { "S7MBC4", 3, 0, 0, 0 }, NONE } },
    { T("normal", "...and the Confederation needs your help."), { { "S7MBC1", 1, 1050, 0, 0 }, NONE } },            // (after "I'm Admiral Terrell,")
    { T("normal", "And if you ever change your mind about serving..."), { { "S7MBC4", 12, 1700, 0, 0 }, NONE } },   // (after "But I wish you good luck anyway.")
    { T("normal", "To sweeten the deal, we'll throw in..."), { { "S7MBC1", 29, 0, 1450, 0 }, NONE } },              // (before "30,000")
    { D("...a %s.  Hell of a ship."), { { "SAMEFGHT", 0, 0, 950, 0 }, NONE } },                                 // "Hell of a ship," (before "the Centurion")
    { D("Beautiful, eh?  Look at those hard edges, the aerodynamic sleekness..."), { { "BUYFIGHT", 3, 0, 0, 0 }, NONE } },
    { D("She has racks for 2 guns and a weapon."), { { "BUYTUG", 3, 0, 400, 0 }, { "BUYTUG", 7, 740, 2350, 0 }, NONE } },   // "She has" + "racks for two guns, a weapon"
    { D("Did I mention she's a fast ship?"), { { "BUYMERCH", 4, 0, 1200, 0 }, NONE } },                         // "Did I mention she's a fast ship"
    { D("And it's a steal at $U3 credits."), { { "BUYMERCH", 6, 2700, 3480, 0 }, NONE } },                      // "And it's a steal"
    { D("I can give you $U1 trade in value on your old ship, plus $U2 for all your extras."), { { "BUYFIGHT", 7, 0, 0, 0 }, NONE } },   // "I'll use your old ship as a trade-in, including any extras."
    { D("Leaving you with $U4 credits to outfit your new %s."), { { "BUYFIGHT", 8, 0, 0, 0 }, NONE } },         // "Any credits left will be credited to your account."
    { T("normal", "So...what'll it be?"), { { "S7MBC1", 29, 2450, 0, 0 }, NONE } },
    { kAsk, kTerrell, 0, 0, { NONE } },
    { P("normal", "Deal me in."), { { "S7MBC1", 30, 2300, 0, 0 }, NONE } },                                           // (after "Running has never been my style, Admiral.")
    { D("You'll find your new ship fueled up and waiting on the landing pad for you."), { { "BUYFIGHT", 11, 0, 0, 0 }, NONE } },
    { D("Your payment will be deducted automatically.  Have a good one!"), { { "BUYFIGHT", 12, 0, 0, 0 }, NONE } },
    { T("normal", "...I wish you good luck anyway."), { { "S7MBC4", 12, 150, 1550, 0 }, NONE } },
    { kElse, kTerrell, 0, 0, { NONE } },
    { P("normal", "I'll take my chances on my own."), { { "S7MBC1", 39, 0, 0, 0 }, NONE } },
    { T("normal", "As I said, I won't force you to do this, $NM..."), { { "S7MBC3", 20, 0, 0, 0 }, NONE } },
    { D("Call me if you change your mind."), { { "BUYFIGHT", 14, 0, 0, 0 }, NONE } },
    { kEnd, kTerrell, 0, 0, { NONE } },
};

// The cargo aboard does not fit the new ship's hold: the dealer's own words, which name no ship
const Line kStilettoCargo[] = {
    { T("normal", "And that's not all, $NM."), { { "CREDITS", 42, 0, 1100, 0 }, NONE } },
    { T("normal", "To sweeten the deal, we'll throw in..."), { { "S7MBC1", 29, 0, 1450, 0 }, NONE } },
    { D("...a %s.  Hell of a ship."), { { "SAMEFGHT", 0, 0, 950, 0 }, NONE } },
    { D("Good choice, but far be it for me to pass up a sale..."), { { "CARGO", 0, 0, 0, 0 }, NONE } },
    { D("...I can't afford to take advantage of a customer."), { { "CARGO", 1, 0, 0, 0 }, NONE } },
    { D("The problem is, all of the cargo currently listed on your ships manifest won't fit in this baby..."), { { "CARGO", 2, 0, 0, 0 }, NONE } },
    { D("...and the Merchant's Guild won't let us trade your surplus cargo."), { { "CARGO", 3, 0, 0, 0 }, NONE } },
    { D("So go ahead, sell off some cargo, come back and I'll fix you up."), { { "CARGO", 4, 0, 0, 0 }, NONE } },
    { kEnd, kTerrell, 0, 0, { NONE } },
};

// Too poor: the dealer's own, which speak of "this ship"
const Line kStilettoPoor[] = {
    { T("normal", "And that's not all, $NM."), { { "CREDITS", 42, 0, 1100, 0 }, NONE } },
    { T("normal", "To sweeten the deal, we'll throw in..."), { { "S7MBC1", 29, 0, 1450, 0 }, NONE } },
    { D("...a %s.  Hell of a ship."), { { "SAMEFGHT", 0, 0, 950, 0 }, NONE } },
    { D("I hate to break it to you, but we've checked your account..."), { { "TOOPOOR", 0, 0, 0, 0 }, NONE } },
    { D("...and you don't have enough credits to buy this ship."), { { "TOOPOOR", 1, 0, 0, 0 }, NONE } },
    { D("She sure is a fine ship though, isn't she?"), { { "TOOPOOR", 2, 0, 0, 0 }, NONE } },
    { D("Let's look at the numbers..."), { { "TOOPOOR", 4, 0, 0, 0 }, NONE } },
    { D("This ship retails for $U3 credits."), { { "TOOPOOR", 5, 0, 0, 0 }, NONE } },
    { D("I can give you $U1 trade in for your ship, plus $U2 for your extras."), { { "TOOPOOR", 6, 0, 0, 0 }, NONE } },
    { D("Including your cash on hand, that leaves you short by about $U4 credits."), { { "TOOPOOR", 7, 0, 0, 0 }, NONE } },
    { D("Go get some more cash, and come back when you're ready to deal, okay?"), { { "TOOPOOR", 8, 0, 0, 0 }, NONE } },
    { kEnd, kTerrell, 0, 0, { NONE } },
};

// The player flies one already
const Line kStilettoSame[] = {
    { T("normal", "What're you doing here, $NM?  Thought you'd be gone by now."), { { "S7MBC3", 0, 0, 0, 0 }, NONE } },
    { P("normal", "I'm not that anxious to hit space again."), { { "S7MBC3", 2, 0, 2300, 0 }, NONE } },
    { T("normal", "Now if you would leave me alone, I have a decision to make."), { { "S7MBC3", 22, 0, 0, 0 }, NONE } },
    { kEnd, kTerrell, 0, 0, { NONE } },
};

#undef T
#undef P
#undef D
#undef NONE

struct Talk { const Line *lines; size_t count; };
struct ShipTalk { int ship; Talk convs[4]; };       // (the offer, the cargo, too poor, the same ship)
#define TALK(a) { a, sizeof(a) / sizeof(a[0]) }
const ShipTalk kTalks[] = {
    { 1, { TALK(kStilettoOffer), TALK(kStilettoCargo), TALK(kStilettoPoor), TALK(kStilettoSame) } },
};
#undef TALK
