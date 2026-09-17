# Changelog

## v0.3.2-rc2 - 2026-09-17 (release candidate)

**rc1 of this release was flashed at 17:00 and rolled back within minutes.** It did what it set
out to do and still left about 12 KB less free memory than v0.3.1 - enough that several requests
came back "low memory" during testing and the board came within 156 bytes of nothing at all. rc2
takes most of that back with the four-stop limit below and stops the reply buffer growing on a
board that is already short. Nothing in rc2 has been flashed or measured on a device.

v0.3.1 shipped today at 15:07. By about forty minutes of uptime the owner's board had crumbled
into the same kind of lockout it was built to prevent - and this time it also locked the door,
refusing the page that would have shown why and outlasting the safety net meant to catch it. This
release states that plainly and fixes what it exposed.

- **v0.3.1 collapsed again, and the lockout was worse than the crash it replaced.** Released at
  15:07, the board ran normally for about forty minutes and then free memory fell from its usual
  39 KB to 17-20 KB, with the single largest unbroken piece down to 3,444 bytes - too small for the
  4,864-byte piece every poll reserves up front for the live-arrivals feed, so every poll failed in
  exactly the same place. What actually triggers the crumbling still isn't known; the memory trace
  running at the time only kept the last 90 seconds, and it had already scrolled past by 15:50.
  Worse, the admission rule v0.3.1 itself added - refuse a new connection once memory is this
  fragmented - refused *every* connection once the largest piece dropped under 4,308 bytes,
  including the diagnostics page and the restart button: the two things you would need to see the
  problem or clear it. And the board's own restart-if-stuck safety net didn't fire for over twenty
  minutes, because a failing poll backs off to four minutes between tries - so "fifteen failed
  polls in a row" is about fifty-one minutes, not the few the old code assumed. The board came back
  only after a USB reset.

- **A single request is now always accepted, whatever the memory looks like.** The refusal rule
  above only ever had to protect against several requests competing for memory at once; one
  request alone cannot cause the crash it exists to prevent. It now applies only when the display
  is already busy answering something else - one request at a time gets in regardless, and if its
  own reply will not fit, the board answers "out of memory, try again" instead of going silent.

- **A nightly restart, on by default at 03:30** (Settings -> Device & network, time is editable).
  This board has no way to tidy its own memory while running, and a restart is the only thing that
  does - it takes a few seconds, nothing is lost, and the day starts with memory in one piece. It
  only restarts once the clock has synced, the board has been up over an hour, and never in the
  middle of an update. It is a mitigation and not a cure: the underlying cause of the crumbling is
  still not known, which is what the memory log below is for.

- **Service alerts are now off by default on new setups.** They cost a fetch and held memory
  between polls for something that only ever showed on the web page and in the log. Existing
  displays keep whatever they were already set to.

- **The display now restarts itself after three failed-for-memory polls instead of fifteen** -
  about three and a half minutes instead of the fifty-one the old count actually took once the
  failure backoff is accounted for. It is now counted the moment a poll fails for memory rather
  than inferred afterwards from a separate memory reading, and an older, slower check for a board
  that has stalled without technically erroring stays at fifteen polls as a backstop. A bug that
  could have silently crashed the board while running this very check was also found and removed.

- **A poll can now finish on memory that has crumbled to about 3 KB pieces.** The 4,864-byte piece
  every poll asked for up front for the live-arrivals feed, and the vehicle list a busy route can
  grow to 5.6 KB, are now set aside once when the display starts and reused every cycle instead of
  being asked for fresh each time - sized to the stops actually configured (2,432 bytes for the
  owner's two-stop setup) rather than the worst case. A routine poll now asks for nothing bigger
  than about 1.2 KB. The cost is stated honestly: about 8 KB less free memory at rest, which is the
  trade this release makes deliberately and which the next run on the hardware is the test of.

- **The shared reply buffer no longer grows and shrinks every cycle.** A reply bigger than its
  usual 6 KB allowance used to make the buffer double in size and then get handed back and rebuilt
  at the normal size on the very next poll - repeating that churn every cycle for as long as
  replies stayed big. It now grows once, keeps the larger size, and is capped at 10 KB so a
  runaway reply still cannot become permanent. Nothing is cut short to make this true: a busy-hour
  vehicle list can honestly run 8-9 KB, and the board now tracks the biggest reply it has ever seen
  so that number, not a guess, can set the buffer size later. Two corrections after rc1 was
  measured: the cap is 8 KB, not 10 - the biggest reply the board actually saw was 7,035 bytes -
  and the buffer only keeps the larger size when there is at least 30 KB free at the time. On a
  board that is already short it is handed back, because growing what the display holds
  permanently is the last thing a short board needs.

- **The out-of-memory reply reserve can now come back on a fragmented board.** It used to only
  re-arm once free memory reached 20 KB with a 4.3 KB largest piece - both worse than what the
  board actually had during the 15:50 lockout, so the reserve stayed empty the whole time it was
  needed. It now re-arms at about 13.5 KB free with a 2.3 KB piece, which is reachable even on a
  heap crumbled this badly.

- **The display now shows a maximum of four stops, down from eight.** Four is the number it could
  always actually draw - a fifth panel does not fit the screen's drawing memory and was refused at
  the moment it was needed, with a note on the page. The limit now says so up front: the Settings
  page stops you at four and the display refuses a saved setting with more, instead of accepting
  something it cannot show. It also gives about 4 KB of memory back, because the arrival-history
  tracker no longer keeps room for eight stops. If you have a display configured with more than
  four, remove the extras before updating - the update will otherwise fall back to your previous
  saved settings.

- **Two hours of memory history, so the trigger can finally be caught.** The display keeps one
  line per poll for the last 240 polls - memory free, largest unbroken piece, the lowest either
  touched during that poll, and which optional steps ran - readable at `/api/debug/ui?log=1`. The
  previous instrument held three polls, enough to name the failing step and nowhere near enough to
  find what leads to it. Alongside it: the biggest reply ever received, how fragmented the memory
  is, and how much the schedule and alert caches are holding.

**Known residual:** what actually triggers the fragmentation is still not identified - the
two-hour memory log above exists specifically to catch it in the act next time. And by design,
this release's resting free-memory floor sits about 8.4 KB lower than v0.3.1's - roughly 31-32 KB
where v0.3.1 rested at 39-40 KB - which is the deliberate cost of the fix above. That figure is
stated after measurement, not before it: rc1 claimed 8 KB, counted only the memory the poll cycle
holds, and left out the 4.5 KB the new memory log occupies; the real gap was about 12 KB until the
four-stop limit gave 4 KB of it back.

## v0.3.1 - 2026-09-17

A memory pass on the owner's board after v0.3.0 had been running for a day. Nothing here changes
what the display shows; all of it changes whether it keeps showing it.

- **A board with a real stop list could fall into a reboot loop roughly every ten minutes, and
  under the conditions that caused it, this release stops it.** On the morning of 2026-09-17 the
  owner's own board (two stops on route 17, alerts, weather, per-stop notes, two Indego stations,
  SD logging, polling every 30 s) ran healthy for three to four and a half minutes after boot, then
  a poll failed with "out of memory during fetch" - not because memory was low (36 KB was still
  free) but because the single largest unbroken piece of it had crumbled to 11.7 KB. A poll cycle
  needs about 12 KB in one piece to complete: a 4 KB buffer for the real-time feed, about 4.8 KB
  held in reserve for it, up to 4 KB for a schedule reply plus a full second copy of it, and 3 KB to
  parse it. Once the largest piece falls under what one cycle needs, every later poll fails the same
  way, and after 15 failures in a row the board's own safety net restarts it - so it did, roughly
  every ten minutes, for as long as the condition lasted. The exact trigger was not reproduced, but
  SEPTA's schedule service was answering with the wrong day at the time, which makes the firmware
  retry a stop's schedule up to twelve times every two minutes - the leading suspect, stated as
  such rather than as a certainty. The fix does not ask the board to use less memory; it asks it to
  stop breaking memory into pieces. Six small buffers a poll used to build fresh, one after another,
  are things a board only ever needs one of at a time, so there is now one 6,144-byte buffer handed
  from step to step instead of six separate ones. An earlier attempt (measured on the owner's board
  for five minutes, never shipped) instead made all six permanently resident: it did stop the
  fragmentation, but cost 13 KB off the board's resting free memory to do it, which turned out to be
  a worse trade and was reverted. Measured over 30 minutes and 60 poll cycles on the shipping
  design: free memory at the start of every poll held at 39-40.7 KB (v0.3.0 started around 40.5 KB
  but was trending down over time), the largest free piece never dropped below 22.5 KB, the
  real-time feed step now costs about 6 KB of free memory instead of about 10, the Indego step no
  longer leaves anything behind between polls (it used to hold onto as much as 17.6 KB until the
  cycle ended), and across the whole run there were zero failed polls, zero out-of-memory stages,
  the number of arrival snapshots alive at once held steady at one, and the background job queue
  never backed up.
- **The web page no longer hangs instead of answering when the board is busy.** Asking the board's
  Stats page for its numbers had no check for low memory, and the request sat in a two-slot queue
  that was only looked at again once the board had 16 KB free with 12 KB of it in one piece - so on
  a board that was already struggling, the request never got any answer at all, and it tied up one
  of the two queue slots for good. Both Stats routes now get the same "not enough memory right now,
  try again" check every other heavy request already had, and the queue itself is now always looked
  at and always answers something, one way or another, instead of sometimes just sitting there.
- **Replying "out of memory" could itself run the board out of memory and crash it - found from a
  saved crash report, not from a live board - and now can't.** When a request failed because memory
  was too tight, the board's own reply saying so needed about 100 bytes to build, and on a board
  that tight, even that could fail, with nothing left to catch it - the one kind of crash this
  firmware's usual safety nets cannot reach. The fix sets aside 1 KB before Wi-Fi even starts and
  holds it in reserve for exactly this, so an "out of memory" reply can (almost) always be built. If
  it still can't be, the connection is now closed politely instead of crashing the board, and the
  web app already treats a closed connection as worth retrying.
- **Settings changed in this release, all owner-approved:** the drawing buffer on the 3.5" boards
  shrank from a twentieth of the screen to a thirtieth (frees 5,120 bytes of memory); each stop now
  keeps track of 8 upcoming trips instead of 12 (frees about 3.6 KB; a stop whose feed shows more
  than eight trips at once will drop the ones farthest out first); the Indego bike-share count now
  refreshes every 10 minutes instead of every 5 (it is the single most expensive thing a poll does);
  and LVGL's drawing-layer scratch space shrank from 24 KB to 8 KB. The screen's overall drawing
  memory pool is unchanged at 36 KB, specifically so four-stop configurations keep fitting - a
  four-stop arrivals page needs about 31.7 KB of it. Alerts, mDNS and the crowding display are
  unchanged.
- **A burst of concurrent web requests could still crash the board, and this is a fourth kind of
  out-of-memory crash neither fix above can reach.** The on-device suite that fires seven requests
  at once (four status checks, a stops lookup, a 30-day usage report and the page script) found it:
  the web server library assembles a reply's list of headers *after* our own code has already
  returned - deep inside its own request-parsing, on a call stack with no handler of ours anywhere
  above it - so when that small allocation failed with memory already at zero, the board crashed
  rather than answering. Both the 1 KB reserve and the nested catch described above only cover
  allocations our own code makes; this one is the library's own, and nothing written in this
  firmware can catch it. (v0.3.0 never hit this, because its usage-report request used to get stuck
  in the queue and never actually ran under load; the fix for that, above, means it now does - and
  that combination is what exposed this one.) The fix turns away new connections before that can
  happen: an incoming connection is refused immediately, before anything is allocated for it, once
  free memory drops under about 7.9 KB, the largest free piece drops under about 4.3 KB, or more
  than 5 replies are already being built at once. The web app already retries a connection that gets
  refused this way, so a very busy board may briefly turn away extra requests instead of crashing.
- **The Stats/proxy background job no longer starts in the middle of one of those bursts.** Its
  check was "is there enough memory right now" - true at the very start of a burst, a moment before
  several requests already in flight claim their share of it. The job now also waits for those
  in-flight requests to clear before starting, and a job that has to wait keeps its place at the
  front of the queue rather than being dropped; if it is still waiting after about 6 seconds it
  answers "try again" instead of waiting indefinitely.
- The screen code's 12 remaining uses of LVGL's older show/hide/click/scroll calls, which log a
  warning every time they run, are now switched to LVGL's current dedicated calls. No behavior
  change - it clears the "no warnings on the serial console" check the on-device suite counts. (The
  host-side screen simulator pins an older LVGL version than the boards build against, and that
  older version has no dedicated calls to switch to, so a small compatibility header keeps the
  simulator building. That version gap between simulator and board predates this change and is
  unrelated to it.)
- New numbers on the device's own diagnostics page (`GET /api/debug/ui`): byte-addressable free
  memory and its lowest point since boot, a per-stage memory trace, how many arrival snapshots are
  alive at once, counts of failed and stuck polls, how deep the background job queue is, how much
  headroom is left on each task's stack, whether the emergency out-of-memory reply reserve is
  currently held, how many replies were dropped because even that reserve wasn't enough, whether
  releasing the Bluetooth radio's memory at boot actually worked and how much it returned (confirmed
  on the device: +4,028 bytes), and - new with the burst-crash fix above - how many replies are
  being built right now, how many incoming connections have been turned away, and the cap that
  triggers a refusal.
- **Known residual:** during the 400 KB Indego bike-share download, free memory still dips as low
  as 2,220 bytes at its worst moment since boot (v0.3.0: 1,452 B; an earlier, unshipped attempt at
  this same fix: 696 B). It has not caused a failure in any run so far and is called out here so it
  is not mistaken for a new problem if it is seen again.

## v0.3.0 - 2026-09-16

### Release-candidate fixes (shipped in v0.3.0; written up under "Unreleased" at the time)

Seven defects found reading the v0.3.0 release candidate end to end. Four of them could restart or
boot-loop a display in someone's home; the rest are safety nets that were not quite doing what
their comments said.

- **The new "restart if fetching has stopped" safety net could restart a perfectly healthy display,
  over and over.** It judged the display by whether a whole round of fetching had *finished*, and
  gave that five minutes. But on a network that has gone quiet without saying so - the internet
  down while the Wi-Fi still works, a hotel or café sign-in page, a bad connection - one round can
  legitimately take far longer than that: every request waits out its own timeouts, and there is
  one request per stop, several times over. A display with six or more stops on such a network
  could never finish a round at all, so the net restarted it mid-round, forever, until the network
  came back. Two changes: the display now also reports in every time it *starts* a request, not
  only when it finishes a round, so "is it still going?" is answered by what it is actually doing;
  and three layers of the fetching code that each retried the same dead request independently now
  stop as soon as the network itself is unreachable, instead of multiplying (one stop's timetable
  could cost twelve full attempts a round; it now costs one). The retrying that exists for a real
  reason - SEPTA sometimes answering with the wrong day's timetable - is untouched. A display with
  the maximum eight stops on a blackholing network now keeps working through it, and a display that
  has genuinely frozen is still restarted within about five minutes, whatever its stop list.
- **Firmware uploads could restart the display instead of installing.** Two separate things. The
  groundwork that makes the device survive running out of memory has to be done once per internal
  worker, early, and the upload path ran before the place that does it — so on a display whose
  first-ever web request was a firmware upload, running out of memory during that upload was fatal
  rather than a polite refusal. And the older memory safety net had no pause for uploads, so a
  display already near its threshold could restart itself part-way through writing the new image.
  Neither could damage a display (the new image is only switched to once it has arrived complete),
  but both threw the upload away at the worst possible moment. Both fixed.
- **The check that stops a hostile web page from reaching your display now runs before the display
  writes anything, not just before it answers.** For a firmware upload it used to run afterwards,
  so a rebound request was refused — but only after the device had already written up to 1.7 MB to
  its spare memory. Not something an attacker could actually reach (uploads need the PIN, which a
  web page cannot supply from another site), but the check is worth having where it claims to be.
- **A display could boot-loop on a stop list it had happily accepted.** Stops can each ask for one
  to four arrival rows, and a four-row panel takes about three times the drawing memory of a
  one-row one. The guard that stops the arrivals page running out of memory measured the biggest
  panel it had built so far and assumed the next one was no bigger — so a list that happens to put
  the small stops first could let a big one in with nowhere near enough room. Since which stops are
  shown changes by time of day, the same settings could be fine in the morning and crash in the
  evening — and because it crashes while drawing the first screen at switch-on, with the settings
  still saved, the only way out was a USB cable. The guard now scales its estimate by how many rows
  the next panel actually wants, and the "N more stops will not fit" line at the bottom is now
  reserved up front rather than squeezed out of what is left.
- **The display no longer waits on the fetching to draw itself - anywhere - and the crash that
  caused is gone.** An earlier fix in this same release made the Device info screen stop waiting on
  the fetcher, and was written up as if that were the whole problem. It was one instance of it.
  Testing the release candidate found the same pattern in seven more places and crashed a display
  through one of them: the bike
  counts on the arrivals page asked the fetcher for its numbers, waited half a second for an answer
  while the fetcher was busy, and the display restarted. The weather in the header, the weather note
  under each stop, the SD card line, the statistics and the arrivals themselves were all still
  written the same way; only two had been fixed. Now the screen never waits for the fetcher at all -
  it either gets an answer immediately or redraws the one it had a second ago. The most a busy
  moment can cost is one second of staleness on one line, and there is no wait left that could turn
  into a restart. That includes the two places the earlier fix had already touched: shortening the
  wait turned out to make this particular crash *more* likely rather than less, so "waits a
  twentieth of a second" was not a smaller version of the fix, and they now wait not at all like
  everything else.

  The other half of the same fix is that the fetcher stops making the screen wait in the first
  place. It used to copy the whole list of arrivals twice - once to hand it over, once for the
  screen to pick up - with the screen locked out for the duration of both. It now hands over a
  reference instead of a copy. The screen is quicker, and the display has meaningfully more memory
  free from one minute to the next, which is the thing that was making the web page answer "busy,
  try again" after a few hours up.
- **When the display restarts because it ran out of drawing memory, it now says so.** That restart
  used to be indistinguishable from an ordinary one, which is unhelpful precisely when it matters:
  a stop list that does not fit crashes the same way on every switch-on, so what you have is a boot
  loop with no explanation. The web page can now ask why it last restarted and get "ran out of
  screen memory", with the numbers.
- **The web page no longer gets stuck on "Connecting to the display…" forever.** It retries a busy
  display indefinitely, which is right — a busy display answers in a second or two. But it did the
  same for answers that will never change, the common one being opening the page at the display's
  *old* name after renaming it. Those now stop, and say what happened and what to do about it.
- Documentation: four memory thresholds in DESIGN.md still quoted the pre-release round numbers
  after the code had moved to exact ones, and two places disagreed about how many internal workers
  the device has. Both corrected, with a note that thresholds are quoted in bytes from now on.
- **Asking for statistics could restart the display.** Reading a month of the display's own arrival
  log off the SD card takes five to eight seconds, and the display did all of it in one go without
  once pausing to let its own health check run. The chip watches for exactly that - a job that hogs
  it for five seconds is treated as a hung program - and restarts the board. It only ever happened
  under the test suite, which asks for statistics far faster than the web page does, but the margin
  was thin enough that a slow SD card or a long month could have found it. The reading now pauses
  for a millisecond every forty, which nobody can see and the health check can, and the answer is
  sent as it is written rather than being assembled in memory first. That pause lives in the one
  piece of code every kind of log reading goes through, so a future feature that reads the log gets
  it without anyone having to remember. The display also now reports the longest it has gone
  without pausing, so if this ever starts creeping back it shows up as a number instead of as a
  restart.
- **A web request could restart the display when memory was very tight.** Not one of ours - the web
  server library copies a small piece of bookkeeping on its way into every request, and if the
  display is too short of memory for even that copy, the copy fails in a way nothing is allowed to
  catch, which ends the program. There is no hook anywhere on that path to catch it from, and
  patching the library is not something this project does. What there is, though, is a way to make
  the library skip that step entirely: it only does the copy when something has been registered on
  that path, and this display had one thing registered there - the check that stops a hostile web
  page from reaching it. That check has moved to a different, supported place that runs *earlier*,
  before the request body is even read, so the risky step no longer happens at all. Two things fall
  out of it: a rebound firmware upload is now stopped before a single byte is written rather than
  after, and every request does two or three fewer small memory allocations, which is a small help
  to the memory fragmentation that is still an open problem on long-running displays.
- **The display now says on its serial console when someone has been locked out.** Five wrong PINs
  in a row lock every setting-changing action for thirty seconds. That has always worked, but the
  line that recorded it was switched off in release builds, so a display that locked out said
  nothing about it - and a test run that hit the lockout with no wrong PIN anywhere in it had no
  evidence to go on. The lockout now always prints, and each wrong PIN prints the page it was aimed
  at, so the next time this happens it names the culprit instead of leaving a count.

- **The display now notices when it has quietly stopped fetching arrivals, and restarts itself.**
  It already restarted itself when its memory got so chopped up that every fetch failed — but that
  safety net worked by counting *failed* fetches, so it could only see a display that was still
  trying. If the fetching stopped dead instead (stuck deep inside the network code, which is what
  happened on a test board on 16 September), nothing was counting, nothing complained, and the
  screen sat there showing arrival times that were quietly getting older and older until the board
  eventually fell over on its own. Now the part of the display that draws the screen keeps an eye
  on the part that fetches, and if no fetch has finished — successfully *or* unsuccessfully — for
  several times the normal gap between them, it restarts. It waits at least five minutes before
  doing that, waits longer if you've set a slow refresh, holds off entirely while a firmware update
  is uploading, and gives itself extra time just after switching on. A display that simply has no
  internet is *not* affected: a fetch that fails is still a fetch that finished, so nothing
  restarts and it keeps retrying as before. Your settings are saved on the device, so a restart
  loses nothing. If it does happen, the display says so afterwards — the web page can now ask it
  why it last restarted, and it answers in plain words with the numbers involved.

- **Tapping through the screens no longer risks restarting the display.** The screen has its own
  small, fixed pot of drawing memory, separate from everything else, and the old way of switching
  pages built the next screen before letting go of the one it was leaving - so for a moment it was
  holding two or three at once. On the owner's own two stops, the second tap (from Statistics to
  Device info) asked for more than was left. Each page is now let go of before the next one is
  built, so only one is ever held, and there is room to spare on every configuration. The arrivals
  page also comes back to the front by itself if a page ever cannot be drawn, so a tap can never
  leave the display stuck on Statistics or Device info.
- The display can hold about four stops on the 3.5" screens and six on the 2.4"/2.8" ones. Asking
  for more used to be accepted and then fail; the arrivals page now shows as many as fit and says
  `2 more stops will not fit in this display's memory` at the bottom, so nothing disappears
  silently and the display keeps working.
- Web page: it used to go completely blank if you opened it (or it was already open and
  quietly refreshing in the background) at the moment the display was too busy to answer -
  something that happens on purpose several times a minute, by design (see the HTTPS/memory
  note below), and isn't a sign anything is wrong. Now the page always keeps showing whatever
  it last knew, quietly tries again a few times over the next several seconds, and shows a
  small "the display is busy right now, retrying..." note at the top instead of going blank or
  showing a scary red error. The very first time you open the page, if the display happens to
  be busy right then, you get a calm "Connecting to the display..." message instead of nothing.
  On the Settings page specifically: anything you've typed is never thrown away by this, and if
  Save happens to land at a busy moment you're told to just press it again rather than the
  change silently vanishing.
- Firmware updates over the web no longer get turned away while the display is busy, and the
  memory figures the device reports are now the honest ones. The device has two kinds of free
  memory: a region only usable in whole-word chunks, which nothing here can put a buffer or a
  piece of text into, and the ordinary kind, which is what everything actually uses. Every safety
  check and every number on screen had been counting both together, which made the device look
  like it had 33 KB more room than it really had. Three consequences, all now fixed:
  - **Updating over the web used to fail for no good reason.** On 2026-09-16 every attempt was
    refused for ten minutes while the display was fetching arrivals, and plugging in a cable was
    the only way through. The check was demanding a 16 KB run of free memory when the update
    actually needs 4 KB, and its other half moved up and down with the arrival fetches. Measured
    across 114 samples of normal operation, updates would have been refused 34% of the time
    before, and 4% now - and the 4% that remain are moments when memory is genuinely tight, which
    is what the check is for. Confirmed on the device: four uploads attempted in the exact window
    that used to be refused were all accepted.
  - **Two other safety checks had quietly not been running at all.** The ones protecting the
    status page and the background statistics work were set to thresholds that the miscounted
    figure could never fall below, so half of each check was dead. They now use the real number,
    at levels worked out from what each job actually needs. One visible result: while a firmware
    update is uploading, the web app's status data pauses for a few seconds and then resumes. That
    is the repaired check doing its job - during an upload it steps back so the update gets the
    memory it needs, because a failed update costs far more than a status reading arriving a few
    seconds late. The old build carried on answering through an upload only because the check was
    broken, so this is the bug going away rather than anything being taken out. The web app rides
    it out and keeps showing the last reading; if you have your own script polling the device,
    have it retry rather than treat it as an error.
  - **"Heap free" in the web app and on the device's own info page** now show the memory that can
    really be used. Expect this to read about 33 KB lower than before; nothing got worse, the old
    number was just counting memory that was never available. Anything reading the device's data
    feed sees the original figure unchanged, with the honest one added alongside it.
  - **If an update is ever refused, the message now tells you what to do about it** - restart the
    display and upload again within the first minute - and reports exact byte counts instead of
    rounding to the nearest kilobyte. The rounding mattered: a v0.2.0 device that was twelve bytes
    short of the old limit reported itself as "15 KB below 16 KB", which reads like something is
    using up a whole kilobyte rather than like a coin toss.

  **If you are running the released v0.2.0 on another device, read this.** On the owner's stop
  configuration, v0.2.0 settles twelve bytes below the old update limit and then refuses every
  update over the web, indefinitely. Restarting it clears the way, but only for about 45 seconds
  before it settles back, so the upload has to follow the restart immediately. Updating over the
  USB cable always works and is the reliable escape. Devices updated to this release are not
  affected - the new limit has real room in it rather than twelve bytes. Whether any particular
  v0.2.0 device hits this depends on its stops and which feeds it has switched on; it was measured
  on one configuration, not predicted for all of them.
- HTTPS to the data sources (SEPTA, Open-Meteo, Indego), measured on the owner's board and kept
  as a prototype. A firmware built with `-DTRANSIT_HTTPS` (the `cyd-*-https` envs) asks for a
  verified, encrypted connection on every fetch, with each source pinned to its one root
  certificate, and falls back to plain HTTP only when the device's memory cannot hold an encrypted
  session right now - never because a certificate failed to verify, which simply fails the fetch
  and is retried on the next poll. The web app shows which one the latest arrivals came over
  ("Data link" on the Now page) and offers the choice under Settings, Data & weather ("Data
  connection"); both appear only on a firmware that actually has HTTPS in it.
  **Shipping builds are unchanged and keep fetching over plain HTTP**, because over 21 polls on
  the owner's board the encrypted connection was never once affordable: all 64 fetches that asked
  for it were turned down, with about 29 KB of usable memory free against the 68 KB a session
  needs. The memory the device *reports* as free overstates what a buffer can actually use by
  about 34 KB - a region of the chip that only whole-word reads can reach - and the boot log and
  the poller heartbeat now print both figures side by side. We also checked whether simply doing
  the encrypted fetch at a quieter moment in the poll would help: the whole poll cycle is flat to
  within 6 KB, so the best moment is still about 27 KB short, and at that moment the two 16.7 KB
  buffers an encrypted session needs cannot both be found even with every other cost set to zero.
  DESIGN.md 2.1 records the numbers and what it would take (a rebuilt vendor library with smaller
  encryption buffers, or a board with extra memory). Nothing about the arrivals changed during the
  test: both stops stayed live and the poll cadence was the same as the shipping build.
  The Open-Meteo and Indego addresses are now written as `https://` like SEPTA's, which changes
  nothing in a shipping build. The HTTPS build also gives the poller task 2 KB more stack and
  prints a per-stage memory trace.
- Firmware update over the web: the curl example needed `-H "Expect:"`; without it the upload is
  dropped and the device stays on its old build (documented in firmware/README.md).

- Memory: the firmware now gives the C++ runtime a 2 KB emergency exception pool, so an
  out-of-memory error can always be caught and answered with a 503 or a skipped frame instead
  of rebooting the device (this used to be the one uncatchable case, DESIGN.md 12.1). Done by
  supplying the pool-size hook from the firmware itself - no SDK rebuild. `POST /api/debug/oom`
  (PIN) proves it on the device: it drains the heap, forces the failure, and reports
  `caught:true`. One gap in that turned up the same day and is fixed too: the very first
  out-of-memory error on each background task needed a scrap of memory of its own, which is not
  there by definition at that moment, so on a freshly booted device the first failure still
  rebooted it. Each task now claims that scrap at startup while memory is plentiful, and the
  on-device check runs right after a reboot so it actually tests the first-failure case.
  `/api/state` and `/api/config` are streamed straight into the network buffers instead of being
  built as a second copy first, lowering the peak memory of the busiest request.
- Screen: the Wi-Fi signal in the top strip is four bars filled in solid, like a phone's status
  bar, instead of a symbol with a number beside it. The same bars appear on the device page.
- Screen: the statistics page now looks like part of the arrivals page - the same top strip, one
  card per stop with its route badge and title - and shows the on-time percentage as a big
  number over a filled bar (green, amber or red), with the average lateness in the arrival
  badge colours, the worst hour and the ghost count as labelled tiles, and the arrival count
  underneath. The card adapts to every board: two stops on the 3.5" board get the full layout,
  four stops on a 2.4" board get one line each, nothing is clipped.
- Screen: the device page is arranged as a Network card (signal bars, network name, the web
  address, IP address, and whether the last SEPTA update worked) and a card named after the
  device (the web PIN in large digits, SD card status - with a warning when log rows are not
  being written - free memory and uptime), with the hold-to-reset-Wi-Fi button along the bottom.
  Where the screen is tall enough (the 3.5" board in either orientation) a third card, Data
  sources, lists each feed that is switched on - SEPTA, weather, Indego, service alerts - with
  ok / stale / failed in the arrival colours and how long ago it was last fetched, so "is it
  working?" has a one-glance answer. These two pages are now built when you tap to them and freed when you leave, so a four-stop
  setup fits in the display's memory.
- Web: the Settings page is arranged into blocks of settings that affect each other (Screen &
  appearance, Schedules, Alerts & reminders, Data & weather, Device & network, Web PIN,
  Firmware & maintenance) with a one-line intro each; a setting that only matters when another
  is on sits under it and is dimmed while it is off; jump pills and a "Filter settings" box find
  a setting by name or description; one sticky save bar shows unsaved changes with Undo; reboot
  and Wi-Fi reset sit in a fenced "Restart and reset" area with plain-language hints.

## v0.2.0 - 2026-09-16

An external adversarial review of v0.1.2 (32 findings) drove this pass. HTTPS to SEPTA remains
deferred to its own release; firmware signing was deliberately not added.

- Security: state-changing requests (save settings, reboot, Wi-Fi reset, firmware update, PIN
  change, log downloads) need a per-device PIN sent as `X-Pin`; viewing stays open. The PIN is
  generated on first boot, printed once on the serial console and shown on the device info screen;
  change it from Settings. Five wrong tries lock the routes for 30 s. Requests with a foreign
  `Host` header are refused (DNS-rebinding defence); the page cannot be framed.
- Setup Wi-Fi: the first-boot network now has a per-device WPA2 password shown on the screen with
  a QR code. A device that already has Wi-Fi saved never reopens setup by itself: it keeps retrying
  its network and only a tap on the screen opens setup, which closes again after 10 idle minutes.
  Setup rejects overlapping connection attempts and escapes SSIDs correctly.
- Firmware updates: an upload without a file, a second concurrent upload, a disconnect mid-file,
  an oversized image, too little or too fragmented heap, and an image built for a different board
  are each refused with a specific error; the device only reboots after the image verified.
- Settings: every value is type- and range-checked before it is narrowed (256 no longer wraps to
  brightness 0); strings are length-capped and control characters rejected; profile stop lists
  must be unique; Regional Rail lines are normalised to their code (`Paoli/Thorndale` becomes
  `PAO`). Saves go through a temporary file, are read back and verified, and the previous file is
  kept as `/config.prev.json` for recovery (`config_recovered` in `/api/state`). The device name
  and time zone apply immediately on save. The panel honours each stop's `show` count.
- Arrivals: a shared stop no longer shows another route's schedule (subway aliases still work);
  skipped and cancelled trips no longer reappear as scheduled arrivals; truncated or malformed
  feeds, a failed schedule for one stop and a failed rail station now report per-stop health
  (`live`, `schedule_only`, `stale`, `unavailable`) instead of looking like "nothing due"; a feed
  older than five minutes is marked stale rather than shown as live. Every hand-written number
  parser is bounded (a malformed rail status used to overflow). Retained feed data is capped.
- Statistics: a failed poll can no longer create arrivals, ghosts, no-shows or headways; a
  scheduled trip that turns live is reconciled instead of later counted as a no-show; the
  tracker keeps the soonest trips rather than churning; headways are computed in time order and
  never negative; on-time % is computed over arrivals with known lateness (null when none) with
  the counts shown; "prediction accuracy" is now "forecast stability" and arrival rows record how
  they were inferred; outages count from the first failure and include ongoing ones; the
  overview reserves slots for the currently configured stops and reports exclusions.
- Log file: the header now matches the 21-column rows (schema v3, `temp_c` in Celsius); records
  are bounded so the reader can always read what the writer wrote; blank lines are skipped;
  downloads re-emit a mixed month in one schema with spreadsheet-safe text; short SD writes are
  counted (`sd.dropped_rows`, `sd.write_ok`) and a removed card is reported; one download at a
  time so the poller keeps its file handle.
- Poller: arrivals are published as soon as they are fetched; alerts, weather, Indego and logging
  run afterwards on their own timers and cannot delay the next poll; a peer trickling bytes is cut
  off by an absolute deadline; proxied setup responses are leased until the browser finished
  reading them; the on-device stats page no longer scans the SD card on the display task.
- Weather and Indego: failed weather fetches back off (1, 5, 10 min) and a forecast older than an
  hour is hidden rather than shown as current; Indego rows are only logged from a fresh sample and
  unknown counts stay unknown.
- Web: the stop-map tooltip no longer renders agency text as HTML; Leaflet loads with subresource
  integrity; the wizard offers Regional Rail lines by name but stores the code, and asks bus vs
  trolley explicitly; a live prediction without lateness data is labelled "live", not "sched";
  the Now page shows each stop's health; the Indego request uses HTTPS; every setting has a
  plain-language hint (including what each crowding word and icon means); the PIN is asked for
  once and remembered by the browser.

## v0.1.2 - 2026-09-15

- Logging: every arrival row now records SEPTA's crowding estimate, the weather at the event
  (temperature and WMO code) and whether an alert or detour applied to the route; one `bike`
  row per Indego station per hour records bikes, e-bikes and free docks. Old 14-column rows
  still parse (DESIGN.md 9.1).
- Stats: `/api/stats` gains crowding by hour and weekday and an expected-wait / reliability
  by-hour table; new `/api/stats/overview` compares every stop (and charts Indego availability
  by hour) in one pass. The web Stats page shows all of it, with an all-stops table on top.
- Web: the Indego card on the Now page is styled like the stop panels.
- Fixed a boot-time crash: Arduino's first hostname lookup clears lwIP's DNS cache from the
  caller's task, and if SNTP's own lookup was in flight the SNTP callback asserted without the
  core lock. The firmware now resolves the NTP host once before starting SNTP.
- Header weather is a colour icon (sun, moon, cloud-sun, cloud-moon, cloud, rain, showers, snow,
  fog, storm; `firmware/tools/gen_weather_icons.py`) plus the temperature; the words remain on the
  night page. Web assets are referenced with their content hash so a browser never keeps an old
  page after an update.
- The optional HTTPS mode (`device.use_https`, `device.tls_verify`, the CA bundle) is gone: it
  was off by default, the board cannot afford a TLS session's RAM, every service the firmware
  uses serves plain http, and dropping it frees ~100 KB of flash. Old configs with those keys
  still load.

## v0.1.1 - 2026-09-14

- Crowding: the word for SEPTA's "many seats" level is now `open` (it used to be a bare `seats`,
  which read as a truncated label). `device.show_crowding` becomes `device.crowding` (off / words /
  icons / both) plus `device.crowding_icons` (seats = chairs then people, crowd = people only); old
  configs are migrated on load. The web Now page shows the same word or meter.
- Crowding icons were invisible on the panel (the recolor colour prefix lost its trailing space
  to a too-small buffer, so LVGL laid the label out at zero width); the person glyph is now a
  wider silhouette. `/api/debug/ui` reports each row's label state (`rows`).
- Indego: a proper section on the panel (`[bicycle] Indego` header with a stale-age note, one row
  per station with bicycle / bolt / dock counts, red at 0 and amber at 1-2) and `bike.style`
  (icons | words). `/api/state` bike stations gain `classic` and `total_docks`; the web Now page
  card matches.
- Flasher page lists every service the firmware talks to.

## v0.1.0 - 2026-09-14

First release. Verified on an ESP32-3248S035R (3.5" resistive Cheap Yellow Display); the other
five Sunton variants build in CI but have not been tested on hardware.

- Live SEPTA bus/trolley arrivals from GTFS-RT joined with TransitView lateness; schedule rows
  from BusSchedules with the wrong-service-day defence; Regional Rail via the Arrivals API;
  subway schedule-only.
- Service alerts and detours ticker (height, speed, and what it shows are settings).
- Weather from Open-Meteo in the header and per-stop notes when the forecast at the next arrival
  is worth mentioning.
- Time-to-leave alert: blinking row, LED, optional speaker chime.
- Crowding (SEPTA seat estimate), commute profiles, alternative stops on long waits, quiet hours
  with wake-on-touch, night clock page, large-text mode, per-stop titles, light and dark themes,
  rotation.
- Indego bike-share counts for up to three stations.
- SD-card logging of predictions and arrivals with on-device statistics (on-time %, lateness by
  hour and weekday, headway/bunching, ghost buses, prediction accuracy) and CSV download.
- Web UI served by the device: stop wizard with map, stats charts, every setting, OTA update.
- Wi-Fi setup hotspot, mDNS (`transit-display.local`), browser flasher on GitHub Pages.
