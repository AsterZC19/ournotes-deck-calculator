#!/usr/bin/env node
/**
 * Thin CLI adapter / bridge to Haneoka Gekisou Engine.
 * Licensed under MPL-2.0.
 */

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';

import { compileFromMasterTables } from './engine/master.ts';
import { gekisoChart, gekisoContext, scoreOrdersGekiso } from './engine/gekiso.ts';
import { gekisoSearch } from './engine/gekiso-objective.ts';
import { resolveSlotSkill } from './engine/skills.ts';
import { performer } from './engine/full/deck.ts';
import { resolveBox, emptyPlayer } from './engine/box.ts';
import { teamPower, setCustomTeamPower } from './engine/power.ts';
import { calcTeamPower } from './engine/deckcalc-power.ts';

const ALL_36_TABLES = [
  'MasterCharacter',
  'MasterLiveMusic',
  'MasterLiveMusicScore',
  'MasterMemberCard',
  'MasterMemberCardAwake',
  'MasterMemberCardLevel',
  'MasterMemberCardRank',
  'MasterSupportCard',
  'MasterSupportCardLevel',
  'MasterSupportCardRank',
  'MasterCharacterRank',
  'MasterCharacterTotalRank',
  'MasterVipRankBonus',
  'MasterLiveSkill',
  'MasterLiveSkillEffect',
  'MasterGekisouSkill',
  'MasterGekisouSkillEffect',
  'MasterLeaderSkill',
  'MasterLeaderSkillEffect',
  'MasterSupportSkill',
  'MasterSupportSkillEffect',
  'MasterGekisouSupportSkill',
  'MasterGekisouSupportSkillEffect',
  'MasterSkillTarget',
  'MasterSkillCondition',
  'MasterSkillConditionSet',
  'MasterSkillCumulativeCondition',
  'MasterLiveNoteParameter',
  'MasterLiveComboScoreBonus',
  'MasterLiveGekisouRankingScoreBonus',
  'MasterLiveGekisouLuckBasePoint',
  'MasterLiveGekisouLuckBonusLot',
  'MasterParameter',
  'MasterLiveSettings',
  'MasterLiveJudgementParameter',
  'MasterLiveJudgementTiming',
];

const SUPPORTED_EFFECT_TYPES = new Set([
  0, 1000, 1001, 1002, 1003, 1500, 1501, 1502, 1503, 2000, 2001, 2002, 2003, 2004, 2005, 3000,
  3001, 3002, 3003, 3004, 4004, 11000, 11001, 11002, 11003, 11004, 11005, 12000, 12002, 12003,
  12004, 12006, 13000, 13002, 13003, 13004, 13005, 15000,
]);

function readJsonInput(filePath) {
  if (filePath && filePath !== '-') {
    return JSON.parse(fs.readFileSync(filePath, 'utf8'));
  }
  return JSON.parse(fs.readFileSync(0, 'utf8'));
}

function loadMaster(masterDir, tablesMap) {
  const missing = [];
  for (const t of ALL_36_TABLES) {
    if (tablesMap && tablesMap[t]) continue;
    if (masterDir && fs.existsSync(path.join(masterDir, t + '.json'))) continue;
    missing.push(t);
  }
  if (missing.length > 0) {
    throw new Error(`缺少必需的 Master 数据表 (${missing.length}/36 张缺失): ${missing.join(', ')}`);
  }

  function readTable(name) {
    if (tablesMap && tablesMap[name]) {
      const data = tablesMap[name];
      return Array.isArray(data) ? data : data._allData || data.entries || data;
    }
    if (masterDir) {
      const p = path.join(masterDir, name + '.json');
      if (fs.existsSync(p)) {
        const content = JSON.parse(fs.readFileSync(p, 'utf8'));
        return Array.isArray(content) ? content : content._allData || content.entries || content;
      }
    }
    return null;
  }
  return compileFromMasterTables({ server: 'jp', releaseId: 'deckcalc' }, readTable);
}

function normalizeChartSource(chartInput) {
  let raw = chartInput;
  if (typeof chartInput === 'string') {
    raw = JSON.parse(fs.readFileSync(chartInput, 'utf8'));
  }

  if (raw.format === 'nnnotes.live-score/1') {
    return {
      durationMs: (raw.lastNoteTimeMs || 0) + 1000,
      notes: raw.notes.map((n) => ({
        timeMs: n.timeMs,
        operateType: n.op,
        judgementType: 0,
        judged: !!n.judgement,
      })),
      skillTimesMs: raw.skill.map((s) => s.timeMs),
      feverMs: (raw.fever || []).map((f) => [f.start.timeMs, f.end.timeMs]),
      enumeration: raw.notes.map((n) => ({
        op: n.op,
        timeMs: n.timeMs,
        judgementType: 0,
      })),
    };
  }

  if (Array.isArray(raw.notes) && Array.isArray(raw.skill_times_ms)) {
    const lastTime = Math.max(...raw.notes.map((n) => n.t || n.timeMs || 0), 0);
    const fevers = (raw.fevers || raw.fever || []).map((f) =>
      Array.isArray(f) ? f : [f.start_ms ?? f.start?.timeMs ?? 0, f.end_ms ?? f.end?.timeMs ?? 0],
    );
    const notes = raw.notes.map((n, idx) => ({
      id: idx + 1,
      timeMs: n.t ?? n.timeMs,
      operateType: n.op,
      judgementType: 0,
      judged: n.scoring ?? n.judgement ?? true,
    }));
    return {
      durationMs: raw.duration_ms || lastTime + 1000,
      notes,
      skillTimesMs: raw.skill_times_ms,
      feverMs: fevers,
      enumeration: notes.map((n) => ({
        op: n.operateType,
        timeMs: n.timeMs,
        judgementType: 0,
      })),
    };
  }

  throw new Error('未知的谱面输入格式');
}

function resolveProblemInput(input) {
  if (input.problem) {
    const prob =
      typeof input.problem === 'string'
        ? JSON.parse(fs.readFileSync(input.problem, 'utf8'))
        : input.problem;
    input.problemObj = prob;

    const scoreMode = prob.score_mode ?? prob.meta?.score_mode ?? prob.meta?.source?.score_mode;
    if (!scoreMode || scoreMode !== 'gekisou') {
      throw new Error(
        `输入 problem 缺少或未启用激奏模式: score_mode === 'gekisou' 必须满足 (当前: '${scoreMode}')`,
      );
    }

    input.chart = input.chart || prob.chart;
    input.song = input.song || prob.song;
    input.play_level = input.play_level ?? prob.chart?.level;
    input.missions = input.missions ?? prob.song?.gekisou_missions;
    if (!input.missions || !Array.isArray(input.missions) || input.missions.length !== 3) {
      throw new Error(`激奏必须包含恰好 3 个有效任务 (song.gekisou_missions)`);
    }
    for (const m of input.missions) {
      if (typeof m !== 'number' || !Number.isInteger(m) || m < 1 || m > 4) {
        throw new Error(`任务 ID 必须为 1..4 的整数，收到: ${m}`);
      }
    }

    input.master_tables = input.master_tables || prob.gekisou_master_tables;
    input.master_dir = input.master_dir || prob.meta?.source?.master_dir;
    if (!input.candidates && prob.catalog) {
      input.candidates = {
        members: prob.catalog.members,
        snaps: prob.catalog.snapshots,
      };
    }
  }
}

function validateChartFevers(chartSource) {
  if (!chartSource.feverMs || !Array.isArray(chartSource.feverMs) || chartSource.feverMs.length < 1 || chartSource.feverMs.length > 3) {
    throw new Error(
      `激奏谱面必须包含 1 到 3 个 Fever 区间 (chart.fevers)，当前: ${chartSource.feverMs ? chartSource.feverMs.length : 0}`,
    );
  }
  for (const f of chartSource.feverMs) {
    if (!Array.isArray(f) || f.length !== 2) {
      throw new Error('Fever 区间必须为 [startMs, endMs] 格式');
    }
    const [startMs, endMs] = f;
    if (typeof startMs !== 'number' || typeof endMs !== 'number' || !Number.isFinite(startMs) || !Number.isFinite(endMs)) {
      throw new Error(`Fever 时间必须为有限数值: [${startMs}, ${endMs}]`);
    }
    if (startMs >= endMs) {
      throw new Error(`Fever 起始时间必须严格小于结束时间: [${startMs}, ${endMs}]`);
    }
    if (startMs < 0) {
      throw new Error(`Fever 起始时间必须非负: [${startMs}, ${endMs}]`);
    }
  }
  const sortedF = [...chartSource.feverMs].sort((a, b) => a[0] - b[0]);
  for (let i = 1; i < sortedF.length; i++) {
    if (sortedF[i][0] < sortedF[i - 1][1]) {
      throw new Error(`Fever 区间不能重叠: [${sortedF[i - 1]}] 与 [${sortedF[i]}]`);
    }
  }
}

function setupCustomPower(prob) {
  if (!prob) return;
  setCustomTeamPower((master, player, members, snaps, leader, music, eventEffects) => {
    const rawMembers = members.map((m) => {
      const cat = prob.catalog?.members?.find((c) => c.id === m.card.id);
      const trained = [
        Math.round(m.own[0] / 10000),
        Math.round(m.own[1] / 10000),
        Math.round(m.own[2] / 10000),
      ];
      return {
        id: m.card.id,
        character: m.card.characterId,
        band: m.card.bandId,
        card_type: m.card.cardType,
        tags: [...m.card.bestTags],
        trained,
        event_bonus_bp: cat?.event_bonus_bp || 0,
        card_rank_bonus_bp: cat?.card_rank_bonus_bp || {
          music_type: m.musicTypeRate,
          music_tag: m.musicTagRate,
        },
        leader_skill: cat?.leader_skill || m.card.leaderSkillId,
        leader_skill_level: m.leaderSkillLevel || 5,
        gekisou_skill: cat?.gekisou_skill || m.card.gekisoSkillId,
        live_skill: cat?.live_skill || m.card.liveSkillId,
      };
    });
    const rawSnaps = snaps.map((s) => {
      if (!s) return null;
      const cat = prob.catalog?.snapshots?.find((c) => c.id === s.card.id);
      const trained = s.percent
        ? [s.percent[0], s.percent[1], s.percent[2]]
        : cat?.trained || [0, 0, 0];
      return {
        id: s.card.id,
        card_type: s.card.cardType,
        trained,
        event_bonus_bp: cat?.event_bonus_bp || 0,
        card_rank_bonus_bp: cat?.card_rank_bonus_bp || {
          type_link: s.typeLinkRate,
        },
      };
    });
    const res = calcTeamPower(prob, leader, rawMembers, rawSnaps);
    return { total: res.total, slots: res.slots, leaderPercents: [] };
  });
}

function validateSkillSupport(master, members, snaps) {
  const issues = { unsupported: [] };
  for (const m of members) {
    if (m.card.gekisoSkillId) {
      const gSkill = master.gekisoSkills.get(m.card.gekisoSkillId);
      if (!gSkill) {
        issues.unsupported.push(`unknown-gekisou-skill:${m.card.gekisoSkillId}`);
      } else {
        for (const ef of gSkill.effects) {
          if (!SUPPORTED_EFFECT_TYPES.has(ef.type)) {
            issues.unsupported.push(`gekisou-skill:${m.card.gekisoSkillId}:effect:${ef.type}`);
          }
        }
      }
    }
  }
  for (const s of snaps) {
    if (!s) continue;
    for (const sid of s.card.gekisoSupportSkillIds) {
      if (!sid) continue;
      const gSupport = master.gekisoSupportSkills.get(sid);
      if (!gSupport) {
        issues.unsupported.push(`unknown-gekisou-support-skill:${sid}`);
      } else {
        for (const ef of gSupport.effects) {
          if (!SUPPORTED_EFFECT_TYPES.has(ef.type)) {
            issues.unsupported.push(`gekisou-support-skill:${sid}:effect:${ef.type}`);
          }
        }
      }
    }
  }
  return issues;
}

function handleScore(input) {
  resolveProblemInput(input);
  const prob = input.problemObj;
  const master = loadMaster(input.master_dir, input.master_tables);
  const chartSource = normalizeChartSource(input.chart);
  validateChartFevers(chartSource);

  const playLevel = input.play_level || 26;
  const missions = input.missions;

  const assumedPlacement = input.options?.assumed_placement ?? 1;
  if (assumedPlacement < 1 || assumedPlacement > 5) {
    throw new Error(`无效的 assumed_placement: ${assumedPlacement} (必须在 1..5 范围内)`);
  }

  const accuracy = input.options?.accuracy || { great: 0, just: 100 };
  const gChart = gekisoChart(master, chartSource, playLevel, missions, accuracy, assumedPlacement);

  const formation = input.formation || {};
  const rawSlots = formation.slots || [];
  if (rawSlots.length !== 5) {
    throw new Error(`队伍必须包含恰好 5 个槽位，当前数量: ${rawSlots.length}`);
  }

  // Normalize slots and leader
  const normalizedSlots = rawSlots.map((s, idx) => ({
    member: s.member ?? s.member_id,
    snapshot: s.snapshot ?? s.snap_id ?? 0,
    trigger: s.trigger ?? s.trigger_index ?? idx,
    member_growth: s.member_growth,
    snap_growth: s.snap_growth,
  }));

  const leaderId = formation.leader ?? null;
  if (leaderId !== null) {
    const leaderIdx = normalizedSlots.findIndex((s) => s.member === leaderId);
    if (leaderIdx === -1) {
      throw new Error(`指定的队长卡片 ${leaderId} 不在编队槽位中`);
    }
    if (leaderIdx !== 0) {
      const tmp = normalizedSlots[0];
      normalizedSlots[0] = normalizedSlots[leaderIdx];
      normalizedSlots[leaderIdx] = tmp;
    }
  }

  const allowMissingSnaps =
    prob.experimental_allow_missing_snapshots === true ||
    input.options?.allow_missing_snapshots === true;

  // Validate cards exist in catalog if catalog is provided
  const catalogMembers = prob?.catalog?.members ? new Map(prob.catalog.members.map((m) => [m.id, m])) : null;
  const catalogSnaps = prob?.catalog?.snapshots ? new Map(prob.catalog.snapshots.map((s) => [s.id, s])) : null;

  for (const s of normalizedSlots) {
    if (catalogMembers && !catalogMembers.has(s.member)) {
      throw new Error(`编队成员 ${s.member} 不在输入卡池 (catalog) 中`);
    }
    if (s.snapshot > 0 && catalogSnaps && !catalogSnaps.has(s.snapshot)) {
      throw new Error(`编队 Snapshot ${s.snapshot} 不在输入卡池 (catalog) 中`);
    }
    if (s.snapshot <= 0 && !allowMissingSnaps) {
      throw new Error(`编队槽位缺少 Snapshot (常规模式每槽必须绑定 Snapshot)`);
    }
    if (s.member_growth) {
      for (const [k, v] of Object.entries(s.member_growth)) {
        if (typeof v !== 'number' || v < 1 || !Number.isInteger(v)) {
          throw new Error(`成员成长参数 ${k} 必须为 >= 1 的整数，收到 ${v}`);
        }
      }
    }
    if (s.snap_growth) {
      for (const [k, v] of Object.entries(s.snap_growth)) {
        if (typeof v !== 'number' || v < 1 || !Number.isInteger(v)) {
          throw new Error(`Snapshot 成长参数 ${k} 必须为 >= 1 的整数，收到 ${v}`);
        }
      }
    }
  }

  const memberInputs = normalizedSlots.map((s, idx) => {
    const cat = catalogMembers?.get(s.member);
    return {
      key: `m_${idx}_${s.member}`,
      cardId: s.member,
      level: s.member_growth?.level ?? cat?.level ?? null,
      awake: s.member_growth?.awake ?? cat?.awake ?? null,
      rank: s.member_growth?.rank ?? cat?.rank ?? null,
      liveSkillLevel: s.member_growth?.live_skill_level ?? cat?.live_skill_level ?? null,
      gekisoSkillLevel: s.member_growth?.gekisou_skill_level ?? cat?.gekisou_skill_level ?? null,
    };
  });

  const snapInputs = [];
  const slotSnapKeys = [];
  normalizedSlots.forEach((s, idx) => {
    const snapId = s.snapshot;
    if (snapId > 0) {
      const cat = catalogSnaps?.get(snapId);
      const key = `s_${idx}_${snapId}`;
      snapInputs.push({
        key,
        cardId: snapId,
        level: s.snap_growth?.level ?? cat?.level ?? null,
        rank: s.snap_growth?.rank ?? cat?.rank ?? null,
      });
      slotSnapKeys.push(key);
    } else {
      slotSnapKeys.push(null);
    }
  });

  const box = resolveBox(master, memberInputs, snapInputs, 'max');
  if (box.unknown.length > 0) {
    throw new Error(`未知的成员或快照卡片: ${box.unknown.join(', ')}`);
  }

  const memberChars = new Set(box.members.map((m) => m.card.characterId));
  if (memberChars.size !== 5) {
    throw new Error(`编队中存在重复角色 (唯一点数: ${memberChars.size})`);
  }
  const snapCards = box.snaps.map((s) => s.card.id).filter((id) => id > 0);
  if (new Set(snapCards).size !== snapCards.length) {
    throw new Error(`编队中存在重复 Snapshot`);
  }

  const snapMap = new Map(box.snaps.map((s) => [s.key, s]));
  const slotSnaps = slotSnapKeys.map((k) => (k ? snapMap.get(k) ?? null : null));

  // Check supported skills
  const issues = validateSkillSupport(master, box.members, slotSnaps);
  const slotSkills = box.members.map((m, i) => resolveSlotSkill(master, m, slotSnaps[i], issues));
  if (issues.unsupported.length > 0) {
    throw new Error(`包含不受支持的技能效果或条件: ${issues.unsupported.join(', ')}`);
  }

  const deck = box.members.map((m, i) => performer(master, m, slotSnaps[i]));

  const chartHasLuck = !!gChart.luck;
  const userSeeds = input.options?.seeds;
  const seeds = userSeeds !== undefined && userSeeds !== null ? userSeeds : [];
  if (!Array.isArray(seeds)) throw new Error('seeds 必须为数组');
  for (const s of seeds) {
    if (typeof s !== 'number' || !Number.isInteger(s) || s < 0) {
      throw new Error(`seeds 元素必须为非负整数: ${s}`);
    }
  }
  const luckMode = !chartHasLuck
    ? 'nominal_model_exact'
    : seeds.length === 0
    ? 'nominal_expectation'
    : 'sample_mean';

  const ctx = gekisoContext(master, gChart, deck, seeds);

  setupCustomPower(prob);
  let power = input.power;
  let slotPowers = null;
  if (power == null) {
    const player = input.player || emptyPlayer();
    const music = prob?.song
      ? {
          musicType: prob.song.type,
          bestTags: prob.song.tags || [],
          memoryKey: prob.song.id,
        }
      : null;
    const pRes = teamPower(master, player, box.members, slotSnaps, 0, music);
    power = pRes.total;
    slotPowers = pRes.slots || null;
  }

  const scores = scoreOrdersGekiso(master, gChart, ctx, power, slotSkills);

  return {
    status: 'ok',
    mean: scores.mean,
    max: scores.max,
    min: scores.min,
    max_order: scores.maxOrder,
    min_order: scores.minOrder,
    order_scores: Array.from(scores.scores),
    power,
    slot_powers: slotPowers,
    assumed_placement: assumedPlacement,
    seeds,
    luck_mode: luckMode,
    details: {
      range_percents: gChart.rangePercent,
      span: gChart.span,
      notes_count: gChart.notes.length,
      luck_present: chartHasLuck,
    },
  };
}

function handleRank(input) {
  resolveProblemInput(input);
  const prob = input.problemObj;
  const master = loadMaster(input.master_dir, input.master_tables);
  const chartSource = normalizeChartSource(input.chart);
  validateChartFevers(chartSource);

  const playLevel = input.play_level || 26;
  const missions = input.missions;

  const assumedPlacement = input.options?.assumed_placement ?? 1;
  if (assumedPlacement < 1 || assumedPlacement > 5) {
    throw new Error(`无效的 assumed_placement: ${assumedPlacement} (必须在 1..5 范围内)`);
  }

  const accuracy = input.options?.accuracy || { great: 0, just: 100 };
  const gChart = gekisoChart(master, chartSource, playLevel, missions, accuracy, assumedPlacement);

  const catalogMembers = prob?.catalog?.members ? new Map(prob.catalog.members.map((m) => [m.id, m])) : null;
  const catalogSnaps = prob?.catalog?.snapshots ? new Map(prob.catalog.snapshots.map((s) => [s.id, s])) : null;

  // Validate candidate cards existence and uniqueness
  if (input.candidates) {
    if (input.candidates.members) {
      const seenM = new Set();
      for (const m of input.candidates.members) {
        if (catalogMembers && !catalogMembers.has(m.id)) {
          throw new Error(`候选成员卡 ${m.id} 不在输入卡池 (catalog) 中`);
        }
        if (seenM.has(m.id)) {
          throw new Error(`候选成员卡列表中存在重复 ID: ${m.id}`);
        }
        seenM.add(m.id);
      }
    }
    if (input.candidates.snaps) {
      const seenS = new Set();
      for (const s of input.candidates.snaps) {
        if (catalogSnaps && !catalogSnaps.has(s.id)) {
          throw new Error(`候选 Snapshot ${s.id} 不在输入卡池 (catalog) 中`);
        }
        if (seenS.has(s.id)) {
          throw new Error(`候选 Snapshot 列表中存在重复 ID: ${s.id}`);
        }
        seenS.add(s.id);
      }
    }
  }

  const candidateMembers = (input.candidates?.members || []).map((m, idx) => {
    const cat = catalogMembers?.get(m.id);
    return {
      key: `m_${idx}_${m.id}`,
      cardId: m.id,
      level: m.level ?? m.growth?.level ?? cat?.level ?? null,
      awake: m.awake ?? m.growth?.awake ?? cat?.awake ?? null,
      rank: m.rank ?? m.growth?.rank ?? cat?.rank ?? null,
      liveSkillLevel: m.live_skill_level ?? m.growth?.live_skill_level ?? cat?.live_skill_level ?? null,
      gekisoSkillLevel: m.gekisou_skill_level ?? m.growth?.gekisou_skill_level ?? cat?.gekisou_skill_level ?? null,
    };
  });

  const candidateSnaps = (input.candidates?.snaps || []).map((s, idx) => {
    const cat = catalogSnaps?.get(s.id);
    return {
      key: `s_${idx}_${s.id}`,
      cardId: s.id,
      level: s.level ?? s.growth?.level ?? cat?.level ?? null,
      rank: s.rank ?? s.growth?.rank ?? cat?.rank ?? null,
    };
  });

  const box = resolveBox(master, candidateMembers, candidateSnaps, 'max');
  if (box.unknown.length > 0) {
    throw new Error(`未知的候选卡片: ${box.unknown.join(', ')}`);
  }

  const allowMissingSnaps =
    prob.experimental_allow_missing_snapshots === true ||
    input.options?.allow_missing_snapshots === true;

  if (!allowMissingSnaps && box.snaps.length < 5) {
    throw new Error(
      `候选 Snapshot 数量不足 5 张 (当前: ${box.snaps.length}，常规激奏每槽必须绑定 Snapshot)`,
    );
  }

  // Validate candidate skills
  const issues = validateSkillSupport(master, box.members, box.snaps);
  if (issues.unsupported.length > 0) {
    throw new Error(`包含不受支持的技能效果或条件: ${issues.unsupported.join(', ')}`);
  }

  const player = input.player || emptyPlayer();
  const criterionMap = {
    mean: 'mean',
    max: 'max',
    min: 'min',
    'nominal-mean': 'mean',
    'best-ap': 'max',
    'worst-ap': 'min',
    theoretical_score: 'max',
    mean_score: 'mean',
  };
  const criterion = criterionMap[input.criterion] || 'mean';

  const memberIndexById = new Map(box.members.map((m, idx) => [m.card.id, idx]));
  const snapIndexById = new Map(box.snaps.map((s, idx) => [s.card.id, idx]));

  // Merge problem constraints and CLI constraints
  const probConstraints = prob?.constraints || {};
  const cliConstraints = input.constraints || {};

  if (probConstraints.distinct_characters === false) {
    throw new Error('激奏模式必须满足角色互异 (不支持 distinct_characters=false)');
  }
  if (probConstraints.distinct_snapshots === false) {
    throw new Error('激奏模式必须满足 Snapshot 互异 (不支持 distinct_snapshots=false)');
  }
  if (probConstraints.bindings && Object.keys(probConstraints.bindings).length > 0) {
    throw new Error('激奏组队暂不支持 bindings 约束');
  }
  if (probConstraints.member_pool && probConstraints.member_pool.length > 0) {
    throw new Error('激奏暂不支持 member_pool 约束，请直接通过候选卡池过滤');
  }
  if (probConstraints.snapshot_pool && probConstraints.snapshot_pool.length > 0) {
    throw new Error('激奏暂不支持 snapshot_pool 约束，请直接通过候选卡池过滤');
  }
  if (probConstraints.leader_pool && probConstraints.leader_pool.length > 1) {
    throw new Error('激奏暂不支持多选队长池 (leader_pool 长度 > 1)');
  }
  if (probConstraints.leader_candidates && probConstraints.leader_candidates.length > 1) {
    throw new Error('激奏暂不支持多候选队长 (leader_candidates 长度 > 1)');
  }

  const rawReqM =
    (cliConstraints.required_members?.length ? cliConstraints.required_members : null) ??
    probConstraints.required_members ??
    [];
  const rawExcM =
    (cliConstraints.excluded_members?.length ? cliConstraints.excluded_members : null) ??
    probConstraints.excluded_members ??
    probConstraints.banned_members ??
    [];
  const rawReqS =
    (cliConstraints.required_snapshots?.length
      ? cliConstraints.required_snapshots
      : cliConstraints.required_snaps?.length
      ? cliConstraints.required_snaps
      : null) ??
    probConstraints.required_snapshots ??
    probConstraints.required_snaps ??
    [];
  const rawExcS =
    (cliConstraints.excluded_snapshots?.length
      ? cliConstraints.excluded_snapshots
      : cliConstraints.excluded_snaps?.length
      ? cliConstraints.excluded_snaps
      : null) ??
    probConstraints.excluded_snapshots ??
    probConstraints.excluded_snaps ??
    probConstraints.banned_snapshots ??
    [];
  const rawLeader =
    cliConstraints.leader ??
    probConstraints.leader ??
    (probConstraints.leader_pool?.length === 1 ? probConstraints.leader_pool[0] : null) ??
    (probConstraints.leader_candidates?.length === 1 ? probConstraints.leader_candidates[0] : null);

  // Validate existence of constraint cards in candidates/catalog
  for (const id of rawReqM) {
    if (!memberIndexById.has(id)) throw new Error(`约束中的成员卡不存在: ${id}`);
  }
  for (const id of rawExcM) {
    if (!memberIndexById.has(id)) throw new Error(`约束中的成员卡不存在: ${id}`);
  }
  for (const id of rawReqS) {
    if (!snapIndexById.has(id)) throw new Error(`约束中的 Snapshot 不存在: ${id}`);
  }
  for (const id of rawExcS) {
    if (!snapIndexById.has(id)) throw new Error(`约束中的 Snapshot 不存在: ${id}`);
  }
  if (rawLeader !== null && !memberIndexById.has(rawLeader)) {
    throw new Error(`约束中的队长卡不存在: ${rawLeader}`);
  }

  // Check constraint consistency
  if (rawReqM.length > 5) {
    throw new Error(`必选成员卡数量不能超过 5 张 (当前: ${rawReqM.length})`);
  }
  for (const id of rawReqM) {
    if (rawExcM.includes(id)) {
      throw new Error(`成员卡 ${id} 同时出现在必选和排除列表中`);
    }
  }
  for (const id of rawReqS) {
    if (rawExcS.includes(id)) {
      throw new Error(`Snapshot ${id} 同时出现在必选和排除列表中`);
    }
  }
  if (rawLeader !== null && rawExcM.includes(rawLeader)) {
    throw new Error(`队长卡片 ${rawLeader} 不能在排除列表中`);
  }
  if (rawLeader !== null && rawReqM.length === 5 && !rawReqM.includes(rawLeader)) {
    throw new Error(`必选成员已有 5 人且不包含指定队长 ${rawLeader}`);
  }

  const reqCharIds = rawReqM.map((id) => box.members[memberIndexById.get(id)].card.characterId);
  if (new Set(reqCharIds).size !== rawReqM.length) {
    throw new Error(`必选成员中存在重复角色`);
  }
  if (rawLeader !== null && !rawReqM.includes(rawLeader)) {
    const leaderChar = box.members[memberIndexById.get(rawLeader)].card.characterId;
    if (reqCharIds.includes(leaderChar)) {
      throw new Error(`指定队长 ${rawLeader} 的角色与必选成员角色冲突`);
    }
  }

  const requiredMembers = rawReqM.map((id) => memberIndexById.get(id));
  const excludedMembers = rawExcM.map((id) => memberIndexById.get(id));
  const requiredSnaps = rawReqS.map((id) => snapIndexById.get(id));
  const excludedSnaps = rawExcS.map((id) => snapIndexById.get(id));
  const leaderIdx = rawLeader !== null ? memberIndexById.get(rawLeader) : null;

  setupCustomPower(prob);

  const chartHasLuck = !!gChart.luck;
  const userSeeds = input.options?.seeds;
  const seeds = userSeeds !== undefined && userSeeds !== null ? userSeeds : [];
  if (!Array.isArray(seeds)) throw new Error('seeds 必须为数组');
  for (const s of seeds) {
    if (typeof s !== 'number' || !Number.isInteger(s) || s < 0) {
      throw new Error(`seeds 元素必须为非负整数: ${s}`);
    }
  }
  const luckMode = !chartHasLuck
    ? 'nominal_model_exact'
    : seeds.length === 0
    ? 'nominal_expectation'
    : 'sample_mean';

  const k = input.k || 10;
  if (typeof k !== 'number' || !Number.isInteger(k) || k <= 0) {
    throw new Error(`k (Top-K) 必须为 > 0 的正整数`);
  }
  const timeLimitMs = input.options?.time_limit_ms || 0;
  if (typeof timeLimitMs !== 'number' || timeLimitMs < 0) {
    throw new Error(`time_limit_ms 不能为负数`);
  }

  if (input.oracle) {
    // Complete combinatorial enumeration for small pools
    const numMembers = box.members.length;
    const numSnaps = box.snaps.length;

    // Check pool bounds
    if (numMembers > 10) {
      throw new Error(`Oracle exhaustive search 仅适用于小池 (当前成员数: ${numMembers} > 10)`);
    }

    const availableSnaps = [];
    for (let s = 0; s < numSnaps; s++) {
      if (!excludedSnaps.includes(s)) availableSnaps.push(s);
    }

    // Estimate combination upper bound
    let estimatedCombos = 1;
    let cMembers = 1;
    for (let i = 0; i < 5; i++) cMembers = (cMembers * (numMembers - i)) / (i + 1);
    const leaderWays = leaderIdx !== null ? 1 : 5;
    let snapWays = 1;
    if (availableSnaps.length >= 5) {
      for (let i = 0; i < 5; i++) snapWays *= availableSnaps.length - i;
    } else if (allowMissingSnaps) {
      let sum = 0;
      const M = availableSnaps.length;
      for (let sc = 0; sc <= M; sc++) {
        let comb = 1;
        for (let i = 0; i < sc; i++) comb = (comb * (5 - i)) / (i + 1);
        let perm = 1;
        for (let i = 0; i < sc; i++) perm *= M - i;
        sum += comb * perm;
      }
      snapWays = sum;
    } else {
      throw new Error(`可用 Snapshot 数量不足 5 张 (当前: ${availableSnaps.length})`);
    }
    estimatedCombos = cMembers * leaderWays * snapWays;
    if (estimatedCombos > 50000) {
      throw new Error(
        `Oracle 穷举组合数超过上限 50000 (估算: ${estimatedCombos})，请缩小候选池或使用 search`,
      );
    }

    const validMemberCombos = [];
    function chooseMembers(start, chosen) {
      if (chosen.length === 5) {
        const chars = new Set(chosen.map((idx) => box.members[idx].card.characterId));
        if (chars.size !== 5) return;
        for (const req of requiredMembers) {
          if (!chosen.includes(req)) return;
        }
        for (const exc of excludedMembers) {
          if (chosen.includes(exc)) return;
        }
        if (leaderIdx !== null && !chosen.includes(leaderIdx)) {
          return;
        }
        validMemberCombos.push([...chosen]);
        return;
      }
      for (let i = start; i < numMembers; i++) {
        chosen.push(i);
        chooseMembers(i + 1, chosen);
        chosen.pop();
      }
    }
    chooseMembers(0, []);

    // Generate snap arrangements for available snaps
    const snapPermutations = [];
    function permuteSnaps(cur, chosenIndices) {
      if (cur.length === 5) {
        for (const req of requiredSnaps) {
          if (!cur.includes(req)) return;
        }
        snapPermutations.push([...cur]);
        return;
      }
      if (allowMissingSnaps && availableSnaps.length < 5) {
        cur.push(-1);
        permuteSnaps(cur, chosenIndices);
        cur.pop();
      }
      for (let i = 0; i < availableSnaps.length; i++) {
        const s = availableSnaps[i];
        if (!chosenIndices.has(s)) {
          chosenIndices.add(s);
          cur.push(s);
          permuteSnaps(cur, chosenIndices);
          cur.pop();
          chosenIndices.delete(s);
        }
      }
    }
    permuteSnaps([], new Set());

    const memberSetHits = new Map();
    const music = prob?.song
      ? {
          musicType: prob.song.type,
          bestTags: prob.song.tags || [],
          memoryKey: prob.song.id,
        }
      : null;

    let evaluatedTeams = 0;
    const startTime = Date.now();
    let timedOut = false;

    for (const teamMembers of validMemberCombos) {
      if (timedOut) break;
      const setKey = teamMembers
        .map((idx) => box.members[idx].card.id)
        .sort((a, b) => a - b)
        .join('|');
      let bestForSet = null;

      const leaderCandidates = leaderIdx !== null ? [leaderIdx] : teamMembers;

      for (const lId of leaderCandidates) {
        if (timedOut) break;
        // Reorder members so leader is slot 0
        const orderedMembers = [
          box.members[lId],
          ...teamMembers.filter((idx) => idx !== lId).map((idx) => box.members[idx]),
        ];

        for (const snapIndices of snapPermutations) {
          if (timeLimitMs > 0 && Date.now() - startTime >= timeLimitMs) {
            timedOut = true;
            break;
          }
          const selectedSnaps = snapIndices.map((sIdx) => (sIdx >= 0 ? box.snaps[sIdx] : null));
          const deck = orderedMembers.map((m, i) => performer(master, m, selectedSnaps[i]));
          const sSkills = orderedMembers.map((m, i) => resolveSlotSkill(master, m, selectedSnaps[i]));
          const ctx = gekisoContext(master, gChart, deck, seeds);
          const pw = teamPower(master, player, orderedMembers, selectedSnaps, 0, music).total;
          const scores = scoreOrdersGekiso(master, gChart, ctx, pw, sSkills);

          evaluatedTeams++;
          let key = scores.mean;
          if (criterion === 'max') key = scores.max;
          else if (criterion === 'min') key = scores.min;

          if (!bestForSet || key > bestForSet.score) {
            const memberIds = orderedMembers.map((m) => m.card.id);
            const snapIds = selectedSnaps.map((s) => (s ? s.card.id : 0));
            bestForSet = {
              score: key,
              power: pw,
              leaderId: memberIds[0],
              formation: {
                schema: 'ournotes-deck-formation@1',
                leader: memberIds[0],
                slots: memberIds.map((mId, slotIdx) => ({
                  slot: slotIdx,
                  member: mId,
                  snapshot: snapIds[slotIdx] || 0,
                  trigger: slotIdx,
                })),
                member_ids: memberIds,
                snap_ids: snapIds.map((x) => x || null),
              },
              scores: {
                mean: scores.mean,
                max: scores.max,
                min: scores.min,
              },
            };
          }
        }
      }

      if (bestForSet) {
        memberSetHits.set(setKey, bestForSet);
      }
    }

    const allHits = Array.from(memberSetHits.values()).sort((a, b) => b.score - a.score);
    const hits = allHits.slice(0, k).map((hit, idx) => ({
      rank: idx + 1,
      ...hit,
    }));
    if (!allowMissingSnaps) {
      for (const hit of hits) {
        const snaps = hit.formation?.slots?.map((s) => s.snapshot) || hit.formation?.snap_ids || [];
        if (snaps.length < 5 || snaps.some((id) => !id || id <= 0)) {
          throw new Error('编队槽位缺少 Snapshot (常规模式每槽必须绑定 Snapshot)');
        }
      }
    }

    const searchComplete = !timedOut;
    const optimalityCertified = !timedOut;
    const optimalityScope = timedOut
      ? 'incomplete_oracle'
      : seeds.length === 0 || !chartHasLuck
      ? 'exact_global_oracle'
      : 'exact_seeded_oracle';

    return {
      status: hits.length > 0 ? 'ok' : timedOut ? 'incomplete' : 'ok',
      hits,
      search_complete: searchComplete,
      optimality_certified: optimalityCertified,
      optimality_scope: optimalityScope,
      bound: hits[0]?.score ?? 0,
      stats: { total_teams: evaluatedTeams, member_sets: memberSetHits.size, timed_out: timedOut },
      luck_mode: luckMode,
      seeds,
    };
  }

  // Bound search using upstream engine as candidate generator
  const music = prob?.song
    ? {
        musicType: prob.song.type,
        bestTags: prob.song.tags || [],
        memoryKey: prob.song.id,
      }
    : null;

  const searchTimeLimit = timeLimitMs > 0 ? timeLimitMs : 10000;
  const searchRes = gekisoSearch({
    master,
    player,
    members: box.members,
    snaps: box.snaps,
    music,
    chart: gChart,
    seeds,
    criterion,
    constraints: {
      requiredMembers,
      excludedMembers,
      requiredSnaps,
      excludedSnaps,
      leader: leaderIdx,
      bindings: new Map(),
      noSnaps: false,
      minBonus: null,
    },
    k: Math.max(k, 10),
    timeLimitMs: searchTimeLimit,
  });

  // Re-score each candidate hit using custom power and exact simulation to ensure 100% score consistency
  const evaluatedHits = searchRes.hits.map((hit) => {
    const memberIds = hit.team.members.map((idx) => box.members[idx]?.card.id);
    const snapIds = hit.team.snaps.map((idx) => (idx >= 0 ? box.snaps[idx]?.card.id : 0));
    const orderedMembers = hit.team.members.map((idx) => box.members[idx]);
    const selectedSnaps = hit.team.snaps.map((idx) => (idx >= 0 ? box.snaps[idx] : null));
    const deck = orderedMembers.map((m, i) => performer(master, m, selectedSnaps[i]));
    const sSkills = orderedMembers.map((m, i) => resolveSlotSkill(master, m, selectedSnaps[i]));
    const ctx = gekisoContext(master, gChart, deck, seeds);
    const pw = teamPower(master, player, orderedMembers, selectedSnaps, 0, music).total;
    const scores = scoreOrdersGekiso(master, gChart, ctx, pw, sSkills);

    let key = scores.mean;
    if (criterion === 'max') key = scores.max;
    else if (criterion === 'min') key = scores.min;

    return {
      score: key,
      power: pw,
      formation: {
        schema: 'ournotes-deck-formation@1',
        leader: memberIds[0],
        slots: memberIds.map((mId, slotIdx) => ({
          slot: slotIdx,
          member: mId,
          snapshot: snapIds[slotIdx] || 0,
          trigger: slotIdx,
        })),
        member_ids: memberIds,
        snap_ids: snapIds.map((x) => x || null),
      },
      scores: {
        mean: scores.mean,
        max: scores.max,
        min: scores.min,
      },
    };
  });

  evaluatedHits.sort((a, b) => b.score - a.score);
  const hits = evaluatedHits.slice(0, k).map((hit, idx) => ({
    rank: idx + 1,
    ...hit,
  }));

  if (!allowMissingSnaps) {
    for (const hit of hits) {
      const snaps = hit.formation?.slots?.map((s) => s.snapshot) || hit.formation?.snap_ids || [];
      if (snaps.length < 5 || snaps.some((id) => !id || id <= 0)) {
        throw new Error('编队槽位缺少 Snapshot (常规模式每槽必须绑定 Snapshot)');
      }
    }
  }

  return {
    status: 'ok',
    hits,
    search_complete: false,
    optimality_certified: false,
    optimality_scope: 'heuristic_candidates',
    bound: hits[0]?.score ?? 0,
    stats: searchRes.stats,
    luck_mode: luckMode,
    seeds,
  };
}

function main() {
  const args = process.argv.slice(2);
  const command = args[0];

  let inputPath = null;
  for (let i = 1; i < args.length; i++) {
    if (args[i] === '-i' || args[i] === '--input') {
      inputPath = args[i + 1];
      i++;
    }
  }

  try {
    const input = readJsonInput(inputPath);
    let result;
    if (command === 'score') {
      result = handleScore(input);
    } else if (command === 'rank') {
      result = handleRank(input);
    } else {
      throw new Error(`未知子命令: ${command} (支持 score, rank)`);
    }
    process.stdout.write(JSON.stringify(result, null, 2) + '\n');
  } catch (err) {
    const errOutput = {
      status: 'error',
      message: err.message || String(err),
      stack: err.stack,
    };
    process.stderr.write(JSON.stringify(errOutput, null, 2) + '\n');
    process.exit(1);
  }
}

main();
