#!/usr/bin/env -S octave --no-gui -qf
## KM 4.2 measurement campaign analysis (report: section 3.1.6, stage 7) from hilsim dump CSV files (-o).
##
## Usage: analiza_km42.m --input <dir> --output <dir> [--hil_latency <us>] [--reference <file>]
##
## Files expected in the --input directory (names as in the report):
##   km42-<S>-run<r>.csv      campaign runs, S in {nominal, partitioned, nopart}, r = 1, 2, 3, ...
##   km42-referencyjny.csv    independent reference run in the scenario 1 configuration (y_ref, Annex 5),
##                            another file can be given with --reference
##   km42-kalibracja.txt      uart_latency loopback output (optional): calibration checks and default T_hil
##   km42-kalibracja-dut.csv  hilsim -p echo dump (optional): calibration check of the HIL->DUT->HIL path
##
## Processing time t_i = recv_ns - sent_ns - T_hil, T_hil is --hil_latency, or the average full-packet round trip
## from km42-kalibracja.txt if not given. Responses with negative t_i are ignored, if there are several responses with
## the same idx, only the first one (in dump order) with non-negative t_i is used.
## Row i is correct (c_i = 1) if the dump has a response with the same idx, received before the next row was due
## (recv - sent < period) and all its commands differ from the reference by less than delta = 1e-4. A late answer to
## an earlier row is recorded under the idx of the current row, so it is incorrect unless its value matches.
## C = sum(c_i) / N, N - number of sent rows ('# sent:' metadata, number of reference rows if missing).
##
## Written to the --output directory:
##   km42-wyniki.txt          tables of the report section 3.2 (also printed to stdout)
##   km42-statystyki.csv      statistics of every run
##   km42-niezgodnosci.csv    every incorrect row: idx, reason, expected and received commands
##   km42-ecdf.png            empirical CDF of t_i for the three scenarios (Figure 3)
##   km42-przebieg.png        t_i as a function of the row number (Figure 4)

1; # script file, not a function file

SCENARIOS = {"nominal", "partitioned", "nopart"};
SCEN_NAMES = {"wartości nominalne", "z partycjonowaniem", "bez partycjonowania"};
N_MIN = 1000; # minimal number of sent rows (W1)
DELTA = 1e-4; # correctness tolerance

## Reads hilsim dump: struct with idx, sent, recv [ns], cmd (matrix, one column per command), lat [us] (recv - sent),
## period [ns] and sha (input_sha256 from the metadata).
function d = load_dump(path)
  text = fileread(path);
  lines = strtrim(strsplit(text, "\n"));
  lines = lines(!cellfun(@isempty, lines));

  # '#' lines are run metadata (start time, input file, input sha256)
  isMeta = strncmp(lines, "#", 1);
  d.sha = "";
  d.nSent = NaN;
  for l = lines(isMeta)
    tok = regexp(l{1}, '^#\s*input_sha256:\s*(\S+)', "tokens", "once");
    if (!isempty(tok))
      d.sha = tok{1};
    endif
    tok = regexp(l{1}, '^#\s*sent:\s*(\d+)', "tokens", "once");
    if (!isempty(tok))
      d.nSent = str2double(tok{1});
    endif
  endfor
  lines = lines(!isMeta);
  if (isempty(lines))
    error("%s: no header", path);
  endif

  header = strsplit(lines{1}, ",");
  idxCol = find(strcmp(header, "idx"));
  sentCol = find(strcmp(header, "sent_ns"));
  recvCol = find(strcmp(header, "recv_ns"));
  if (isempty(idxCol) || isempty(sentCol) || isempty(recvCol))
    error("%s: no idx,sent_ns,recv_ns columns", path);
  endif

  ncol = numel(header);
  nrow = numel(lines) - 1;
  vals = sscanf(strjoin(lines(2:end), ","), "%f,");
  if (numel(vals) != ncol * nrow)
    error("%s: malformed rows", path);
  endif
  vals = reshape(vals, ncol, nrow).';

  d.idx = vals(:, idxCol);
  d.sent = vals(:, sentCol);
  d.recv = vals(:, recvCol);
  # commands: idx,sent_ns,recv_ns,cmd0..cmdN, echoes (-p): idx,sent_ns,recv_ns,seq
  d.cmd = vals(:, strncmp(header, "cmd", 3));
  d.lat = (d.recv - d.sent) / 1000;
  d.period = dump_period(d);
endfunction


## Rows are sent periodically, the period [ns] is recovered from send times of answered rows.
function p = dump_period(d)
  if (numel(d.idx) >= 2)
    p = median(diff(d.sent) ./ diff(d.idx));
  else
    p = NaN;
  endif
endfunction


## Drops responses with negative processing time (lat - hilLatency < 0) and keeps only the first remaining response
## for every idx (dump order).
function d = filter_dump(d, hilLatency)
  keep = find(d.lat - hilLatency >= 0);
  [~, first] = unique(d.idx(keep), "first");
  keep = keep(sort(first));
  d.idx = d.idx(keep);
  d.sent = d.sent(keep);
  d.recv = d.recv(keep);
  d.cmd = d.cmd(keep, :);
  d.lat = d.lat(keep);
  d.period = dump_period(d);
endfunction


## Reads uart_latency output: struct with sent, ok, timeouts, mismatches, wire (theoretical time) and full-packet
## round trip min, max, avg [us], empty if the file does not exist.
function k = load_calib(path)
  k = [];
  if (!exist(path, "file"))
    return;
  endif
  text = fileread(path);
  tok = regexp(text, 'packets:\s*(\d+) sent,\s*(\d+) ok,\s*(\d+) timeouts,\s*(\d+) mismatches', "tokens", "once");
  if (isempty(tok))
    error("%s: no packets summary", path);
  endif
  v = str2double(tok);
  k.sent = v(1);
  k.ok = v(2);
  k.timeouts = v(3);
  k.mismatches = v(4);
  tok = regexp(text, 'theoretical wire time:\s*([\d.]+)', "tokens", "once");
  k.wire = NaN;
  if (!isempty(tok))
    k.wire = str2double(tok{1});
  endif
  tok = regexp(text, 'full-packet round trip \[us\]:\s*min\s+([\d.]+)\s+max\s+([\d.]+)\s+avg\s+([\d.]+)', "tokens", "once");
  if (isempty(tok))
    error("%s: no full-packet round trip statistics", path);
  endif
  v = str2double(tok);
  k.min = v(1);
  k.max = v(2);
  k.avg = v(3);
endfunction


## Linear interpolation between closest ranks (numpy default).
function val = quantile_lin(sorted_vals, q)
  pos = (numel(sorted_vals) - 1) * q;
  lo = floor(pos);
  hi = ceil(pos);
  val = sorted_vals(lo + 1) + (sorted_vals(hi + 1) - sorted_vals(lo + 1)) * (pos - lo);
endfunction


## Processing time statistics [us], all samples are used, outliers are not rejected (metric based on the maximum).
function s = lat_stats(t)
  u = sort(t);
  s.n = numel(u);
  if (isempty(u))
    [s.max, s.min, s.mean, s.median, s.std, s.q95, s.q99] = deal(NaN);
    return;
  endif
  s.max = u(end);
  s.min = u(1);
  s.mean = mean(u);
  s.median = median(u);
  if (numel(u) > 1)
    s.std = std(u);
  else
    s.std = 0.0;
  endif
  s.q95 = quantile_lin(u, 0.95);
  s.q99 = quantile_lin(u, 0.99);
endfunction


## Correctness of every reference row, code: 0 - correct, 1 - no response, 2 - late response, 3 - values differ.
## loc is the dump row answering the reference row (0 if none).
function [code, loc] = correctness(run, ref, delta)
  n = numel(ref.idx);
  code = ones(n, 1);
  [found, loc] = ismember(ref.idx, run.idx);
  i = find(found);
  j = loc(found);
  # response after the next row was sent (t_i >= T_in,i+1 - T_in,i)
  late = (run.recv(j) - run.sent(j)) >= run.period;
  # dump has 4 decimal places, difference of 1e-4 may be parsed slightly below delta
  match = all(abs(run.cmd(j, :) - ref.cmd(i, :)) < delta - 1e-9, 2);
  code(i(late)) = 2;
  code(i(!late & !match)) = 3;
  code(i(!late & match)) = 0;
endfunction


function s = fmt(val, prec = 1)
  if (isnan(val))
    s = "nan";
  else
    s = strrep(sprintf("%.*f", prec, val), ".", ",");
  endif
endfunction


## Prints to stdout and to the results file.
function emit(fid, varargin)
  printf(varargin{:});
  fprintf(fid, varargin{:});
endfunction


function s = verdict(ok)
  if (ok)
    s = "SPEŁNIONE";
  else
    s = "NIESPEŁNIONE";
  endif
endfunction


function usage()
  fprintf(stderr, "Usage: analiza_km42.m --input <dir> --output <dir> [--hil_latency <us>] [--reference <file>]\n");
  fprintf(stderr, "  --input <dir>        directory with km42-<S>-run<r>.csv (S: nominal, partitioned, nopart), km42-referencyjny.csv\n");
  fprintf(stderr, "                       and optionally km42-kalibracja.txt, km42-kalibracja-dut.csv\n");
  fprintf(stderr, "  --output <dir>       directory for tables, statistics and plots\n");
  fprintf(stderr, "  --hil_latency <us>   HIL own latency T_hil subtracted from t_i (default: avg from km42-kalibracja.txt)\n");
  fprintf(stderr, "  --reference <file>   reference run dump (default: <input>/km42-referencyjny.csv)\n");
  exit(1);
endfunction


## arguments
args = argv();
inDir = "";
outDir = "";
hilLatency = NaN;
refPath = "";
if (mod(numel(args), 2) != 0)
  usage();
endif
for i = 1:2:numel(args)
  switch (args{i})
    case "--input"
      inDir = args{i + 1};
    case "--output"
      outDir = args{i + 1};
    case "--hil_latency"
      hilLatency = str2double(args{i + 1});
      if (isnan(hilLatency))
        fprintf(stderr, "invalid --hil_latency: %s\n", args{i + 1});
        exit(1);
      endif
    case "--reference"
      refPath = args{i + 1};
    otherwise
      usage();
  endswitch
endfor
if (isempty(inDir) || isempty(outDir))
  usage();
endif
if (isempty(refPath))
  refPath = fullfile(inDir, "km42-referencyjny.csv");
endif
if (!exist(outDir, "dir") && !mkdir(outDir))
  fprintf(stderr, "cannot create %s\n", outDir);
  exit(1);
endif

## calibration (stage 5)
calib = load_calib(fullfile(inDir, "km42-kalibracja.txt"));
calibDutPath = fullfile(inDir, "km42-kalibracja-dut.csv");
calibDut = [];
if (exist(calibDutPath, "file"))
  calibDut = load_dump(calibDutPath);
endif
if (isnan(hilLatency))
  if (isempty(calib))
    fprintf(stderr, "no --hil_latency and no km42-kalibracja.txt in %s\n", inDir);
    exit(1);
  endif
  hilLatency = calib.avg;
endif

## reference and campaign runs
ref = load_dump(refPath);
N = numel(ref.idx);

files = dir(fullfile(inDir, "km42-*-run*.csv"));
runs = struct("scen", {}, "r", {}, "file", {}, "d", {}, "t", {}, "stats", {}, "code", {}, "loc", {}, "C", {}, "nSent", {}, "nSentAssumed", {}, "invalid", {});
for f = files'
  tok = regexp(f.name, '^km42-(\w+)-run(\d+)\.csv$', "tokens", "once");
  if (isempty(tok))
    continue;
  endif
  scen = find(strcmp(SCENARIOS, tok{1}));
  if (isempty(scen))
    fprintf(stderr, "warning: %s: unknown scenario, ignored\n", f.name);
    continue;
  endif
  run.scen = scen;
  run.r = str2double(tok{2});
  run.file = f.name;
  run.d = filter_dump(load_dump(fullfile(inDir, f.name)), hilLatency);
  run.t = run.d.lat - hilLatency;
  run.stats = lat_stats(run.t);
  run.invalid = {};

  if (size(run.d.cmd, 2) != size(ref.cmd, 2))
    run.invalid{end + 1} = sprintf("%d kolumn poleceń, w referencji %d", size(run.d.cmd, 2), size(ref.cmd, 2));
    run.code = ones(N, 1);
    run.loc = zeros(N, 1);
  else
    [run.code, run.loc] = correctness(run.d, ref, DELTA);
  endif

  # the dump has only answered rows, the number of sent rows is taken from '# sent:' metadata if present,
  # otherwise hilsim (without -n, not interrupted) sends all input rows, the same as in the reference
  run.nSentAssumed = isnan(run.d.nSent);
  if (run.nSentAssumed)
    run.nSent = N;
  else
    run.nSent = run.d.nSent;
  endif
  run.C = sum(run.code == 0) / run.nSent;

  # validity criteria (Table 7) checkable from the HIL registers, W3-W9 and W11 require DUT logs
  if (run.nSent < N_MIN)
    run.invalid{end + 1} = sprintf("W1: wystawiono %d porcji danych, wymagane co najmniej %d", run.nSent, N_MIN);
  endif
  if (isempty(run.d.sha) || !strcmp(run.d.sha, ref.sha))
    run.invalid{end + 1} = sprintf("W2: suma kontrolna danych wejściowych %s, w referencji %s", run.d.sha, ref.sha);
  endif
  if (scen == 1 && run.C == 0)
    run.invalid{end + 1} = "W10: brak poprawnych danych w scenariuszu odniesienia";
  endif
  runs(end + 1) = run;
endfor

if (isempty(runs))
  fprintf(stderr, "no km42-<S>-run<r>.csv files in %s\n", inDir);
  exit(1);
endif
# order by scenario, then repetition
[~, order] = sortrows([[runs.scen]', [runs.r]']);
runs = runs(order);
valid = cellfun(@isempty, {runs.invalid});

fid = fopen(fullfile(outDir, "km42-wyniki.txt"), "w");

## input data and calibration
emit(fid, "Zbiór danych wejściowych: %d porcji, okres %s µs, sha256 %s\n", N, fmt(ref.period / 1000), ref.sha);
emit(fid, "Referencja: %s\n", refPath);
emit(fid, "T_hil (opóźnienie własne modułu HIL): %s µs\n\n", fmt(hilLatency));

if (!isempty(calib))
  jitter = calib.max - calib.min;
  ok = calib.timeouts == 0 && calib.mismatches == 0 && calib.min >= calib.wire && jitter < 100;
  emit(fid, "Kalibracja - pętla zwrotna (km42-kalibracja.txt)\n");
  emit(fid, "Pakiety\t%d wysłane, %d poprawne, %d timeouty, %d niezgodności\n", calib.sent, calib.ok, calib.timeouts, calib.mismatches);
  emit(fid, "Teoretyczny czas transmisji [µs]\t%s\n", fmt(calib.wire));
  emit(fid, "Min / średnia / maks. [µs]\t%s / %s / %s\n", fmt(calib.min), fmt(calib.avg), fmt(calib.max));
  emit(fid, "Rozrzut (maks. - min.) [µs]\t%s\n", fmt(jitter));
  emit(fid, "Kryterium akceptacji\t%s\n\n", verdict(ok));
endif
if (!isempty(calibDut))
  dutMin = min(calibDut.lat);
  emit(fid, "Kalibracja - tor HIL->DUT->HIL (km42-kalibracja-dut.csv)\n");
  emit(fid, "Odebrane ramki echa\t%d\n", numel(calibDut.lat));
  emit(fid, "Min. czas obiegu [µs]\t%s\n", fmt(dutMin));
  if (!isempty(calib))
    ok = numel(calibDut.lat) >= N_MIN && dutMin >= calib.wire && dutMin < 1.1 * calib.avg;
    emit(fid, "Min. / średnia pętli zwrotnej\t%s %%\n", fmt(100 * dutMin / calib.avg));
    emit(fid, "Kryterium akceptacji\t%s\n", verdict(ok));
  endif
  emit(fid, "\n");
endif

## rejected runs
emit(fid, "Przebiegi odrzucone:");
if (all(valid))
  emit(fid, " brak\n");
else
  emit(fid, "\n");
  for k = find(!valid)
    emit(fid, "  %s: %s\n", runs(k).file, strjoin(runs(k).invalid, "; "));
  endfor
endif
emit(fid, "Kryteria W3-W9 i W11 sprawdzane na podstawie dziennika DUT, poza skryptem.\n");
if (any([runs.nSentAssumed]))
  emit(fid, "* rejestr nie zawiera liczby wystawionych porcji (# sent:), przyjęto liczbę porcji zbioru wejściowego\n");
endif
emit(fid, "\n");

## scenario tables (Tables 9-11)
stats = fopen(fullfile(outDir, "km42-statystyki.csv"), "w");
fprintf(stats, "scenario,run,valid,sent,responses,t_max_us,t_min_us,t_mean_us,t_median_us,t_std_us,t_q95_us,t_q99_us,correct,C\n");
for k = 1:numel(runs)
  s = runs(k).stats;
  fprintf(stats, "%s,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.6f\n", SCENARIOS{runs(k).scen}, runs(k).r, valid(k),
      runs(k).nSent, s.n, s.max, s.min, s.mean, s.median, s.std, s.q95, s.q99, sum(runs(k).code == 0), runs(k).C);
endfor
fclose(stats);

for scen = 1:numel(SCENARIOS)
  sel = find([runs.scen] == scen);
  if (isempty(sel))
    continue;
  endif
  emit(fid, "Scenariusz %d - \"%s\"\n", scen, SCEN_NAMES{scen});
  emit(fid, "Wielkość");
  for k = sel
    emit(fid, "\tPowtórzenie %d", runs(k).r);
    if (!valid(k))
      emit(fid, " (odrzucone)");
    endif
  endfor
  emit(fid, "\n");
  tab = {
    "Liczba porcji danych", @(r) sprintf("%d%s", r.nSent, repmat("*", 1, r.nSentAssumed));
    "Liczba odpowiedzi", @(r) sprintf("%d", r.stats.n);
    "T_max [µs]", @(r) fmt(r.stats.max);
    "T_min [µs]", @(r) fmt(r.stats.min);
    "T_śr [µs]", @(r) fmt(r.stats.mean);
    "Mediana [µs]", @(r) fmt(r.stats.median);
    "Odch. std. [µs]", @(r) fmt(r.stats.std);
    "Kwantyl 0,95 [µs]", @(r) fmt(r.stats.q95);
    "Kwantyl 0,99 [µs]", @(r) fmt(r.stats.q99);
    "C (poprawność)", @(r) sprintf("%s %% (%d/%d)", fmt(100 * r.C, 2), sum(r.code == 0), r.nSent);
  };
  for row = 1:rows(tab)
    emit(fid, "%s", tab{row, 1});
    for k = sel
      emit(fid, "\t%s", tab{row, 2}(runs(k)));
    endfor
    emit(fid, "\n");
  endfor
  emit(fid, "\n");
endfor

## incorrect rows, for the analysis of every mismatch (section 3.2.6)
REASONS = {"brak odpowiedzi", "odpowiedź po czasie", "wartości niezgodne"};
mis = fopen(fullfile(outDir, "km42-niezgodnosci.csv"), "w");
ncmd = size(ref.cmd, 2);
fprintf(mis, "scenario,run,idx,reason%s%s\n", sprintf(",ref_cmd%d", 0:ncmd - 1), sprintf(",cmd%d", 0:ncmd - 1));
for k = 1:numel(runs)
  for i = find(runs(k).code != 0)'
    fprintf(mis, "%s,%d,%d,%s%s", SCENARIOS{runs(k).scen}, runs(k).r, ref.idx(i), REASONS{runs(k).code(i)}, sprintf(",%.4f", ref.cmd(i, :)));
    if (runs(k).loc(i) > 0)
      fprintf(mis, "%s\n", sprintf(",%.4f", runs(k).d.cmd(runs(k).loc(i), :)));
    else
      fprintf(mis, "%s\n", repmat(",", 1, ncmd));
    endif
  endfor
endfor
fclose(mis);

## parameters R and P for all pairs of valid nominal and partitioned repetitions, the worst one is taken
nom = find(valid & [runs.scen] == 1);
part = find(valid & [runs.scen] == 2);
nopart = find(valid & [runs.scen] == 3);
if (isempty(nom) || isempty(part))
  emit(fid, "Brak ważnych przebiegów scenariusza 1 lub 2, parametry R i P nie zostały wyznaczone\n");
  fclose(fid);
  exit(2);
endif

Rm = zeros(numel(nom), numel(part));
Pm = zeros(numel(nom), numel(part));
for a = 1:numel(nom)
  for b = 1:numel(part)
    Rm(a, b) = 100 * runs(nom(a)).stats.max / runs(part(b)).stats.max;
    Pm(a, b) = 100 * runs(part(b)).C / runs(nom(a)).C;
  endfor
endfor

emit(fid, "R [%%] (wiersze: powtórzenia scenariusza 1, kolumny: powtórzenia scenariusza 2)\n");
emit(fid, "%s\n", strjoin(arrayfun(@(k) sprintf("\tPowtórzenie %d", runs(k).r), part, "UniformOutput", false), ""));
for a = 1:numel(nom)
  emit(fid, "Powtórzenie %d%s\n", runs(nom(a)).r, sprintf("\t%s", cellfun(@(v) fmt(v, 2), num2cell(Rm(a, :)), "UniformOutput", false){:}));
endfor
emit(fid, "\nP [%%] (wiersze: powtórzenia scenariusza 1, kolumny: powtórzenia scenariusza 2)\n");
emit(fid, "%s\n", strjoin(arrayfun(@(k) sprintf("\tPowtórzenie %d", runs(k).r), part, "UniformOutput", false), ""));
for a = 1:numel(nom)
  emit(fid, "Powtórzenie %d%s\n", runs(nom(a)).r, sprintf("\t%s", cellfun(@(v) fmt(v, 2), num2cell(Pm(a, :)), "UniformOutput", false){:}));
endfor
emit(fid, "\n");

## Parameter 1 - Niezawodność (Table 12)
[R, w] = min(Rm(:));
[a, b] = ind2sub(size(Rm), w);
rNom = runs(nom(a));
rPart = runs(part(b));
# R > 100% means the difference is within the measurement spread, the criterion is met
okR = R >= 85;
emit(fid, "Parametr 1 - Niezawodność\n");
emit(fid, "T_max w scenariuszu \"wartości nominalne\" (powtórzenie %d)\t%s µs\n", rNom.r, fmt(rNom.stats.max));
emit(fid, "T_max w scenariuszu \"z partycjonowaniem\" (powtórzenie %d)\t%s µs\n", rPart.r, fmt(rPart.stats.max));
emit(fid, "R = T_max^nom / T_max^part\t%s %%\n", fmt(R, 2));
emit(fid, "Wymagane minimum\t85%%\n");
emit(fid, "Wynik\t%s\n\n", verdict(okR));

## Parameter 2 - Poprawność (Table 13)
[P, w] = min(Pm(:));
[a, b] = ind2sub(size(Pm), w);
pNom = runs(nom(a));
pPart = runs(part(b));
# P > 100% (more correct rows than in the reference scenario) is not a degradation
okP = P >= 100 - 1e-9;
emit(fid, "Parametr 2 - Poprawność\n");
emit(fid, "Liczba porcji poprawnych w scenariuszu \"wartości nominalne\" (powtórzenie %d)\t%d / %d\n", pNom.r, sum(pNom.code == 0), pNom.nSent);
emit(fid, "Poprawność C^nom\t%s\n", fmt(pNom.C, 4));
emit(fid, "Liczba porcji poprawnych w scenariuszu \"z partycjonowaniem\" (powtórzenie %d)\t%d / %d\n", pPart.r, sum(pPart.code == 0), pPart.nSent);
emit(fid, "Poprawność C^part\t%s\n", fmt(pPart.C, 4));
emit(fid, "P = C^part / C^nom\t%s %%\n", fmt(P, 2));
emit(fid, "Wymagana wartość\t100%%\n");
emit(fid, "Wynik\t%s\n\n", verdict(okP));

## partitioning effectiveness, informative, worst case T_max of each scenario
if (!isempty(nopart))
  tNom = max(arrayfun(@(k) runs(k).stats.max, nom));
  tPart = max(arrayfun(@(k) runs(k).stats.max, part));
  tNopart = max(arrayfun(@(k) runs(k).stats.max, nopart));
  emit(fid, "Skuteczność partycjonowania (informacyjnie)\n");
  emit(fid, "T_max^nom / T_max^part / T_max^nopart [µs]\t%s / %s / %s\n", fmt(tNom), fmt(tPart), fmt(tNopart));
  if (tNopart > tNom)
    emit(fid, "eta_iso = (T_max^nopart - T_max^part) / (T_max^nopart - T_max^nom)\t%s %%\n\n", fmt(100 * (tNopart - tPart) / (tNopart - tNom), 2));
  else
    emit(fid, "eta_iso\tnie wyznaczono, T_max^nopart <= T_max^nom\n\n");
  endif
endif

## summary (Table 14)
emit(fid, "Podsumowanie\n");
emit(fid, "Wymaganie\tWartość uzyskana\tWymagane\tStatus\n");
emit(fid, "Parametr 1 - Niezawodność\t%s %%\t>= 85%%\t%s\n", fmt(R, 2), verdict(okR));
emit(fid, "Parametr 2 - Poprawność\t%s %%\t= 100%%\t%s\n", fmt(P, 2), verdict(okP));
for scen = 1:numel(SCENARIOS)
  sel = find(valid & [runs.scen] == scen);
  if (isempty(sel))
    emit(fid, "Wykonanie scenariusza %d po %d porcji danych\tbrak ważnych przebiegów\t%d\t%s\n", scen, N_MIN, N_MIN, verdict(false));
  else
    nSent = min([runs(sel).nSent]);
    emit(fid, "Wykonanie scenariusza %d po %d porcji danych\t%d\t%d\t%s\n", scen, N_MIN, nSent, N_MIN, verdict(nSent >= N_MIN));
  endif
endfor
fclose(fid);

## plots (Figures 3 and 4), valid runs only
COLORS = [0.00 0.45 0.74; 0.85 0.33 0.10; 0.47 0.67 0.19];
STYLES = {"-", "--", ":", "-."};
MARKERS = {".", "x", "+", "o"};
# hidden figures are rendered by the qt toolkit (octave --no-gui), octave-cli fltk cannot print them
try
  h = figure("visible", "off", "position", [0 0 1000 600]);
  hold on;
  names = {};
  for k = find(valid)
    u = sort(runs(k).t);
    stairs(u, (1:numel(u))' / numel(u), STYLES{mod(runs(k).r - 1, numel(STYLES)) + 1}, "color", COLORS(runs(k).scen, :), "linewidth", 1.5);
    names{end + 1} = sprintf("%s, powtórzenie %d", SCEN_NAMES{runs(k).scen}, runs(k).r);
  endfor
  hold off;
  grid on;
  xlabel("Czas przetwarzania t_i [µs]");
  ylabel("Dystrybuanta empiryczna");
  legend(names, "location", "southeast");
  print(h, fullfile(outDir, "km42-ecdf.png"), "-dpng", "-r150");
  close(h);

  h = figure("visible", "off", "position", [0 0 1000 900]);
  for scen = 1:numel(SCENARIOS)
    subplot(numel(SCENARIOS), 1, scen);
    hold on;
    names = {};
    for k = find(valid & [runs.scen] == scen)
      plot(runs(k).d.idx, runs(k).t, MARKERS{mod(runs(k).r - 1, numel(MARKERS)) + 1}, "color", COLORS(scen, :) * (0.4 + 0.6 / runs(k).r), "markersize", 3);
      names{end + 1} = sprintf("powtórzenie %d", runs(k).r);
    endfor
    hold off;
    grid on;
    title(sprintf("Scenariusz %d - \"%s\"", scen, SCEN_NAMES{scen}));
    xlabel("Numer porcji danych");
    ylabel("t_i [µs]");
    if (!isempty(names))
      legend(names, "location", "northeast");
    endif
  endfor
  print(h, fullfile(outDir, "km42-przebieg.png"), "-dpng", "-r150");
  close(h);
catch err
  fprintf(stderr, "warning: plots not generated: %s\n", err.message);
end_try_catch
