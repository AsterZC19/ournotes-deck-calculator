/**
 * Exact power calculation matching ournotes-deck-calculator C++ Engine.
 * Licensed under MPL-2.0.
 */

const f = Math.fround;

export function powerFloor(product: number, float32Mode: boolean = true): number {
  if (float32Mode) {
    const scaled = f(f(product) / 10000.0);
    return Math.floor(scaled);
  }
  return Math.floor(product / 10000.0);
}

function targetMatches(catalog: any, targetId: number, member: any): boolean {
  const target = catalog?.targets?.find((t: any) => t.id === targetId);
  if (!target) {
    throw new Error(`未知技能目标 ID ${targetId}`);
  }
  const checks = [
    !target.character || target.character === member.character,
    !target.band || target.band === member.band,
    !target.card_type || target.card_type === member.card_type,
    !target.tag || (member.tags && member.tags.includes(target.tag)),
  ];
  if (!checks.every(Boolean)) return false;

  if (target.gekisou_mission_type || (target.gekisou_skill_categories && target.gekisou_skill_categories.length > 0)) {
    const gSkill = catalog?.gekisou_skills?.find((s: any) => s.id === member.gekisou_skill);
    if (!gSkill) return false;
    if (target.gekisou_mission_type && target.gekisou_mission_type !== gSkill.mission_type) {
      return false;
    }
    if (target.gekisou_skill_categories && target.gekisou_skill_categories.length > 0) {
      const gCats = gSkill.categories || [];
      const intersects = target.gekisou_skill_categories.some((c: number) => gCats.includes(c));
      if (!intersects) return false;
    }
  }

  if (target.live_skill_categories && target.live_skill_categories.length > 0) {
    const lSkill = catalog?.live_skills?.find((s: any) => s.id === member.live_skill);
    const lCats = lSkill?.categories || [];
    const intersects = target.live_skill_categories.some((c: number) => lCats.includes(c));
    if (!intersects) return false;
  }

  return true;
}

function leaderConditionMatches(
  catalog: any,
  problem: any,
  groupId: number,
  member: any,
  teamMembers?: any[],
): boolean {
  if (!groupId) return true;
  const group = catalog?.condition_groups?.find((g: any) => (g.group ?? g.id) === groupId);
  if (!group) {
    throw new Error(`未知队长技能条件组 ${groupId}`);
  }

  let result = true;
  for (const row of group.rows || []) {
    for (const condId of row) {
      const cond = catalog?.conditions?.find((c: any) => c.id === condId);
      if (!cond) {
        throw new Error(`未知队长技能条件 ${condId}`);
      }
      if (
        cond.type !== 0 &&
        cond.type !== 5000 &&
        cond.type !== 3000 &&
        cond.type !== 3001 &&
        cond.type !== 4012
      ) {
        throw new Error(`队长技能条件类型未支持: ${cond.type}`);
      }

      const matchCandidate = (cand: any) => {
        if (!cond.targets || cond.targets.length === 0) return true;
        return cond.targets.some((tid: number) => targetMatches(catalog, tid, cand));
      };

      let value = true;
      if (cond.type === 5000) {
        value = matchCandidate(member);
      } else if (cond.type === 3000) {
        if (teamMembers) {
          value = teamMembers.some((m) => matchCandidate(m));
        } else {
          value = true;
        }
      } else if (cond.type === 3001) {
        if (teamMembers) {
          value = teamMembers.every((m) => matchCandidate(m));
        } else {
          value = true;
        }
      } else if (cond.type === 4012 && cond.targets && cond.targets.length > 0) {
        value = cond.targets.some((tid: number) => {
          const t = catalog?.targets?.find((x: any) => x.id === tid);
          if (!t || !t.music_type) throw new Error('队长歌曲属性条件缺少 music_type');
          return t.music_type === problem?.song?.type;
        });
      }

      const positive = cond.positive !== false;
      result = result && (positive ? value : !value);
    }
  }
  return result;
}

export function leaderBonusBp(
  problem: any,
  leader: any,
  member: any,
  teamMembers?: any[],
): [number, number, number] {
  const catalog = problem?.catalog;
  const leaderSkill = catalog?.leader_skills?.find((s: any) => s.id === (leader.leader_skill ?? leader.leaderSkillId));
  if (!leaderSkill) return [0, 0, 0];

  const level = leader.leader_skill_level || leader.leaderSkillLevel || 5;
  const effects = (leaderSkill.effects || []).filter((e: any) => e.level === level);

  const axesMap: Record<number, string> = {
    1000: 'all',
    1001: 'technique',
    1002: 'visual',
    1003: 'performance',
    3000: 'all',
    ...(problem.settings?.leader_effect_axes || {}),
  };

  const out: [number, number, number] = [0, 0, 0]; // [performance, technique, visual]

  for (const ef of effects) {
    if (!leaderConditionMatches(catalog, problem, ef.condition_group, member, teamMembers)) {
      continue;
    }

    // Target check
    if (ef.targets && ef.targets.length > 0) {
      const match = ef.targets.some((tid: number) => targetMatches(catalog, tid, member));
      if (!match) continue;
    }

    const val = ef.value || 0;
    const axis = axesMap[ef.effect_type];
    if (!axis) {
      throw new Error(`队长技能 ${leaderSkill.id} 的效果类型 ${ef.effect_type} 未在 settings.leader_effect_axes 中定义`);
    }

    if (axis === 'all') {
      out[0] += val;
      out[1] += val;
      out[2] += val;
    } else if (axis === 'performance') {
      out[0] += val;
    } else if (axis === 'technique') {
      out[1] += val;
    } else if (axis === 'visual') {
      out[2] += val;
    } else {
      throw new Error(`settings.leader_effect_axes.${ef.effect_type} 只支持 'all' / 'performance' / 'technique' / 'visual'，收到 ${axis}`);
    }
  }

  return out;
}

function extraApplies(when: any, member: any, snapshot: any): boolean {
  if (!when || typeof when !== 'object' || Object.keys(when).length === 0) {
    return true;
  }
  if (Array.isArray(when.member_tags_any)) {
    const hit = when.member_tags_any.some((t: number) => member.tags && member.tags.includes(t));
    if (!hit) return false;
  }
  if (Array.isArray(when.member_bands)) {
    if (!when.member_bands.includes(member.band)) return false;
  }
  if (Array.isArray(when.member_characters)) {
    if (!when.member_characters.includes(member.character)) return false;
  }
  if (Array.isArray(when.card_types)) {
    if (!when.card_types.includes(member.card_type)) return false;
  }
  if (Array.isArray(when.snapshot_card_types)) {
    if (!snapshot || !when.snapshot_card_types.includes(snapshot.card_type)) return false;
  }
  return true;
}

export function calcSlotPower(
  problem: any,
  leader: any,
  member: any,
  snapshot: any,
  teamMembers?: any[],
): number {
  const fix = problem.catalog?.fix || {};
  const settings = problem.settings || {};
  const powerModel = settings.power_model || settings.power || {};
  const powerSources = powerModel.sources || powerModel || {};
  const powerRounding = powerModel.rounding || settings.score_model?.rounding || 'float32_floor';
  const float32Mode = powerRounding === 'float32_floor';

  const enableSnapshot = powerSources.snapshot !== false;
  const enableTypeLink = powerSources.type_link !== false;
  const enableBandItem = powerSources.band_item !== false;
  const enableMusicType = powerSources.music_type !== false;
  const enableMusicTag = powerSources.music_tag !== false;
  const enableLeader = powerSources.leader !== false;
  const enableVip = powerSources.vip !== false;
  const extraSources = powerModel.extra_sources || [];

  // Flat character and total rank bonuses
  const crMap = fix.character_rank_bonus_by_character || {};
  const charRank = crMap[String(member.character)] || fix.character_rank_bonus || [0, 0, 0];
  const totalRank = fix.character_total_rank_bonus || [0, 0, 0];
  const flat = [
    charRank[0] + totalRank[0],
    charRank[1] + totalRank[1],
    charRank[2] + totalRank[2],
  ];

  // Common trained power
  const trained = member.trained || [0, 0, 0];
  const eventBonusBp = member.event_bonus_bp || 0;
  const common = [0, 0, 0];
  for (let d = 0; d < 3; d++) {
    const tr = trained[d];
    common[d] = tr + powerFloor(tr * eventBonusBp, float32Mode) + flat[d];
  }

  let total = common[0] + common[1] + common[2];

  // Snapshot power
  if (enableSnapshot && snapshot) {
    const snapTrained = snapshot.trained || [0, 0, 0];
    const snapEventBp = snapshot.event_bonus_bp || 0;
    for (let d = 0; d < 3; d++) {
      const rate = snapTrained[d] + snapEventBp;
      total += powerFloor(common[d] * rate, float32Mode);
    }
  }

  // Type link
  if (enableTypeLink && snapshot && member.card_type === snapshot.card_type) {
    const typeLinkSource =
      settings.type_link_bonus_source === 'snapshot'
        ? snapshot.card_rank_bonus_bp?.type_link || 0
        : member.card_rank_bonus_bp?.type_link || 0;
    const rate = (fix.type_link_base_bp || 0) + typeLinkSource;
    for (let d = 0; d < 3; d++) {
      total += powerFloor(common[d] * rate, float32Mode);
    }
  }

  // Band item
  if (enableBandItem) {
    const bandMap = fix.band_item_bonus_bp_by_band || {};
    const bandRate = bandMap[String(member.band)] || fix.band_item_bonus_bp || [0, 0, 0];
    for (let d = 0; d < 3; d++) {
      total += powerFloor(common[d] * bandRate[d], float32Mode);
    }
  }

  // Music type
  if (enableMusicType && problem.song && problem.song.type === member.card_type) {
    const rate = (fix.music_type_base_bp || 0) + (member.card_rank_bonus_bp?.music_type || 0);
    for (let d = 0; d < 3; d++) {
      total += powerFloor(common[d] * rate, float32Mode);
    }
  }

  // Music tag
  if (enableMusicTag && problem.song && problem.song.tags && member.tags) {
    const tagMatch = problem.song.tags.some((t: number) => member.tags.includes(t));
    if (tagMatch) {
      const rate = (fix.music_tag_base_bp || 0) + (member.card_rank_bonus_bp?.music_tag || 0);
      for (let d = 0; d < 3; d++) {
        total += powerFloor(common[d] * rate, float32Mode);
      }
    }
  }

  // Leader skill
  if (enableLeader && leader) {
    const lrate = leaderBonusBp(problem, leader, member, teamMembers);
    for (let d = 0; d < 3; d++) {
      total += powerFloor(common[d] * lrate[d], float32Mode);
    }
  }

  // VIP
  if (enableVip && fix.vip_bonus_bp) {
    for (let d = 0; d < 3; d++) {
      total += powerFloor(common[d] * fix.vip_bonus_bp[d], float32Mode);
    }
  }

  // Extra rate sources
  for (const src of extraSources) {
    if (src.kind === 'flat') continue;
    if (!extraApplies(src.when, member, snapshot)) continue;
    const rateBp = src.rate_bp || [0, 0, 0];
    for (let d = 0; d < 3; d++) {
      total += powerFloor(common[d] * rateBp[d], float32Mode);
    }
  }

  // Extra flat sources
  for (const src of extraSources) {
    if (src.kind !== 'flat') continue;
    if (!extraApplies(src.when, member, snapshot)) continue;
    const flatPow = src.flat_power || [0, 0, 0];
    for (let d = 0; d < 3; d++) {
      total += flatPow[d];
    }
  }

  return total;
}

export function calcTeamPower(
  problem: any,
  leaderIdx: number,
  members: any[],
  snaps: any[],
): { total: number; slots: number[] } {
  const leader = members[leaderIdx] || members[0];
  const slots = members.map((m, i) => calcSlotPower(problem, leader, m, snaps[i], members));
  const total = slots.reduce((sum, v) => sum + v, 0);
  return { total, slots };
}
