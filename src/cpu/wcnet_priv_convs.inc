// The sales' scripts (wcnet_priv_convs.cpp).  "%s" in a text is the ship's name.  A piece of speech: the game's
// conversation, its line (counted from 0, lines only), from and to in milliseconds (to 0: the line's end), and the
// quiet after it.  What each piece SAYS is in the comment beside it (scripts/privships/convwords.py prints a
// conversation's spoken words and their times).

#define T(mood, text) kLine, kTerrell, mood, text
#define P(mood, text) kLine, kPlayer, mood, text
#define D(text) kLine, kDealer, "normal", text
#define NONE { 0, 0, 0, 0, 0 }

// ---- The Stiletto, from Admiral Terrell (a click on him once his credits have played) --------------------------------
// Every word is Terrell's or the privateer's own (the user, 2026-11-28: "People don't like Terrell offering with the
// ship dealer voice", with the script -- Burrows: "What exactly do you want from me?"  Terrell: "I have an armed ship
// for you.  It's the best ship in the fleet.  Three hundred thousand."  [Beat.]  "So... what'll it be?" -- and its
// cuts).  Words he never says are built from sounds of his: "ship" = the sh of "shot", the i of "kick", the p of
// "help" (the user's "keep" ends unreleased before "running": heard as "chick"); "three" = the thr of "throw", the
// ee of "we"; "hundred" = the h of "help", the un of "Nav one", the dr of "drew", the ed of "suspected".  Each was
// tried until a speech recogniser heard the sentence as written.
#define W_SHIP(gap) { "S7MBC3", 10, 625, 722, -4 }, { "S7MBC1", 34, 2022, 2075, -4 }, { "S7MBC1", 1, 2690, 0, gap }
#define W_THREE { "S7MBC1", 29, 1100, 1200, -4 }, { "S7MBC1", 10, 45, 110, 40 }
#define W_HUNDRED { "S7MBC1", 1, 2520, 2585, -4 }, { "S7MBC1", 32, 3400, 3500, -4 }, { "S7MBC1", 10, 172, 235, -4 }, { "S7MBC1", 7, 1395, 1470, 30 }
// "I have" (before "a decision to make"), "an armed" (before "fleet as chaperone"), "ship", "for you" (before "to lure it")
#define L_IHAVE { T("normal", "I have an armed ship for you: a %s."), { { "S7MBC3", 22, 1780, 2085, 0 }, { "S7MBC3", 9, 2420, 2930, 90 }, W_SHIP(20), { "S7MBC1", 23, 925, 1370, 0 }, NONE } }
// "It's the best" (before "shot you've got"), "ship", "in" (of "we'll throw in"), "the fleet" (before "massed at")
#define L_BEST { T("normal", "It's the best ship in the fleet."), { { "S7MBC3", 10, 0, 622, 0 }, W_SHIP(20), { "S7MBC1", 29, 1310, 1410, 0 }, { "S7MBC3", 4, 900, 1300, 0 }, NONE } }

const Line kStilettoOffer[] = {
    { P("normal", "What exactly do you want from me?"), { { "S7MBC1", 6, 200, 0, 0 }, NONE } },                    // (after "Then")
    L_IHAVE,
    L_BEST,
    // "three", "hundred", then his own "thousand.  So...what'll it be?" with its pause
    { T("normal", "$U3 credits, less $U1 for your old ship and $U2 for its extras.  So...what'll it be?"), { W_THREE, W_HUNDRED, { "S7MBC1", 29, 1840, 0, 0 }, NONE } },
    { kAsk, kTerrell, 0, 0, { NONE } },
    { P("normal", "Deal me in."), { { "S7MBC1", 30, 2300, 0, 0 }, NONE } },                                           // (after "Running has never been my style, Admiral.")
    { T("normal", "Excellent."), { { "S7MBC1", 31, 0, 600, 0 }, NONE } },                                             // (before "This should be the easiest mission")
    { kElse, kTerrell, 0, 0, { NONE } },
    { P("normal", "I'll take my chances on my own."), { { "S7MBC1", 39, 0, 0, 0 }, NONE } },
    { T("normal", "As I said, I won't force you to do this, $NM..."), { { "S7MBC3", 20, 0, 0, 0 }, NONE } },
    { kEnd, kTerrell, 0, 0, { NONE } },
};

// The cargo aboard does not fit the new ship's hold: the privateer asks (his "And" and "my cargo" of "And why should I
// worry about being followed if my cargo's legit?"), the Admiral has no time for it
const Line kStilettoCargo[] = {
    L_IHAVE,
    { P("normal", "And my cargo?  It won't all fit in that."), { { "S1MAC2", 7, 0, 150, 250 }, { "S1MAC2", 7, 2330, 2900, 0 }, NONE } },
    { T("normal", "Then think fast."), { { "S7MBC3", 3, 0, 0, 0 }, NONE } },
    { T("normal", "Now if you would leave me alone, I have a decision to make."), { { "S7MBC3", 22, 0, 0, 0 }, NONE } },
    { kEnd, kTerrell, 0, 0, { NONE } },
};

// Too poor: "you" (of "for you"), "don't have" (of "We don't have a lot of time"), the price
const Line kStilettoPoor[] = {
    L_IHAVE,
    L_BEST,
    { T("normal", "You don't have $U3 credits: you're $U4 short."), { { "S7MBC1", 23, 1080, 1370, 0 }, { "S7MBC1", 0, 100, 400, 0 }, W_THREE, W_HUNDRED, { "S7MBC1", 29, 1840, 2300, 0 }, NONE } },
    { T("normal", "Now if you would leave me alone, I have a decision to make."), { { "S7MBC3", 22, 0, 0, 0 }, NONE } },
    { kEnd, kTerrell, 0, 0, { NONE } },
};

// The player flies one already
const Line kStilettoSame[] = {
    { T("normal", "What're you doing here, $NM?  Thought you'd be gone by now."), { { "S7MBC3", 0, 0, 0, 0 }, NONE } },
    { P("normal", "I'm not that anxious to hit space again."), { { "S7MBC3", 2, 0, 2300, 0 }, NONE } },
    { T("normal", "Now if you would leave me alone, I have a decision to make."), { { "S7MBC3", 22, 0, 0, 0 }, NONE } },
    { kEnd, kTerrell, 0, 0, { NONE } },
};
#undef W_SHIP
#undef W_THREE
#undef W_HUNDRED
#undef L_IHAVE
#undef L_BEST

// ---- The Demon, from the bartender of Tuck's (Sherwood), after his rumour ---------------------------------------------
// The story: the bartender knows of salvage for sale; the fence does the selling in the dealer's words (which fit a
// Demon as they are: "Note the racks for four guns, two weapons"); the bartender wants his tip.

#define B(text) kLine, kBarman, "normal", text
#define Q(text) kLine, kPlayerBar, "normal", text

const Line kDemonOffer[] = {
    { B("Hey mister, have a seat, the first one's on me."), { { "PIRRUM5", 0, 0, 0, 0 }, NONE } },
    { B("There's a lot of loot just floating around out there waiting to be salvaged."), { { "PIRRUM5", 3, 0, 0, 0 }, NONE } },
    { D("A %s.  Hell of a ship."), { { "SAMEFGHT", 0, 0, 950, 0 }, NONE } },                                    // "Hell of a ship,"
    { D("Note the racks for 4 guns, 2 weapons..."), { { "BUYFIGHT", 2, 0, 2200, 0 }, NONE } },                  // "Note the racks for four guns, two weapons" (before "and of course, a rear turret")
    { D("Did I mention she's a fast ship?"), { { "BUYMERCH", 4, 0, 1200, 0 }, NONE } },
    { D("And it's a steal at $U3 credits."), { { "BUYMERCH", 6, 2700, 3480, 0 }, NONE } },
    { D("I can give you $U1 trade in value on your old ship, plus $U2 for all your extras."), { { "BUYFIGHT", 7, 0, 0, 0 }, NONE } },
    { D("Leaving you with $U4 credits to outfit your new %s."), { { "BUYFIGHT", 8, 0, 0, 0 }, NONE } },
    { B("So, what'll it be?"), { { "PIRRUM6", 4, 0, 0, 0 }, NONE } },
    { kAsk, kBarman, 0, 0, { NONE } },
    { Q("Deal me in."), { { "S7MBC1", 30, 2300, 0, 0 }, NONE } },
    { D("You'll find your new ship fueled up and waiting on the landing pad for you."), { { "BUYFIGHT", 11, 0, 0, 0 }, NONE } },
    { D("Your payment will be deducted automatically.  Have a good one!"), { { "BUYFIGHT", 12, 0, 0, 0 }, NONE } },
    { B("Just remember me when it's time to tip."), { { "PIRRUM4", 4, 2300, 0, 0 }, NONE } },                   // (after "No thanks, that's what bartenders do.")
    { kElse, kBarman, 0, 0, { NONE } },
    { Q("I'll take my chances on my own."), { { "S7MBC1", 39, 0, 0, 0 }, NONE } },
    { B("No big deal."), { { "PIRRUM5", 1, 0, 900, 0 }, NONE } },
    { kEnd, kBarman, 0, 0, { NONE } },
};

const Line kDemonCargo[] = {
    { B("There's a lot of loot just floating around out there waiting to be salvaged."), { { "PIRRUM5", 3, 0, 0, 0 }, NONE } },
    { D("A %s.  Hell of a ship."), { { "SAMEFGHT", 0, 0, 950, 0 }, NONE } },
    { D("The problem is, all of the cargo currently listed on your ships manifest won't fit in this baby..."), { { "CARGO", 2, 0, 0, 0 }, NONE } },
    { D("...and the Merchant's Guild won't let us trade your surplus cargo."), { { "CARGO", 3, 0, 0, 0 }, NONE } },
    { D("So go ahead, sell off some cargo, come back and I'll fix you up."), { { "CARGO", 4, 0, 0, 0 }, NONE } },
    { kEnd, kBarman, 0, 0, { NONE } },
};

const Line kDemonPoor[] = {
    { B("There's a lot of loot just floating around out there waiting to be salvaged."), { { "PIRRUM5", 3, 0, 0, 0 }, NONE } },
    { D("A %s.  Hell of a ship."), { { "SAMEFGHT", 0, 0, 950, 0 }, NONE } },
    { D("I hate to break it to you, but we've checked your account..."), { { "TOOPOOR", 0, 0, 0, 0 }, NONE } },
    { D("...and you don't have enough credits to buy this ship."), { { "TOOPOOR", 1, 0, 0, 0 }, NONE } },
    { D("This ship retails for $U3 credits."), { { "TOOPOOR", 5, 0, 0, 0 }, NONE } },
    { D("I can give you $U1 trade in for your ship, plus $U2 for your extras."), { { "TOOPOOR", 6, 0, 0, 0 }, NONE } },
    { D("Including your cash on hand, that leaves you short by about $U4 credits."), { { "TOOPOOR", 7, 0, 0, 0 }, NONE } },
    { D("Go get some more cash, and come back when you're ready to deal, okay?"), { { "TOOPOOR", 8, 0, 0, 0 }, NONE } },
    { kEnd, kBarman, 0, 0, { NONE } },
};

const Line kDemonSame[] = {
    { B("I hope you're gonna buy a drink and not just sit there trawling for a job."), { { "PIRRUM6", 0, 0, 0, 0 }, NONE } },
    { kEnd, kBarman, 0, 0, { NONE } },
};

#undef B
#undef Q
#undef T
#undef P
#undef D
#undef NONE

struct Talk { const Line *lines; size_t count; };
struct ShipTalk { int ship; bool rasp; Who dealerAs; Talk convs[4]; };    // (rasp: the salesman's voice roughened; dealerAs: whose face says his lines; the offer, the cargo, too poor, the same ship)
#define TALK(a) { a, sizeof(a) / sizeof(a[0]) }
const ShipTalk kTalks[] = {
    { 1, false, kTerrell, { TALK(kStilettoOffer), TALK(kStilettoCargo), TALK(kStilettoPoor), TALK(kStilettoSame) } },
    { 2, true, kBarman, { TALK(kDemonOffer), TALK(kDemonCargo), TALK(kDemonPoor), TALK(kDemonSame) } },
};
#undef TALK
