/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "BattlegroundQueue.h"
#include "Arena.h"
#include "BattlegroundMgr.h"
#include "Battleground.h"
#include "Chat.h"
#include "ObjectMgr.h"
#include "Log.h"
#include "Group.h"

/*********************************************************/
/***            BATTLEGROUND QUEUE SYSTEM              ***/
/*********************************************************/

BattlegroundQueue::BattlegroundQueue()
{
    for (uint32 i = 0; i < BG_TEAMS_COUNT; ++i)
    {
        for (uint32 j = 0; j < MAX_BATTLEGROUND_BRACKETS; ++j)
        {
            m_SumOfWaitTimes[i][j] = 0;
            m_WaitTimeLastPlayer[i][j] = 0;
            for (uint32 k = 0; k < COUNT_OF_PLAYERS_TO_AVERAGE_WAIT_TIME; ++k)
                m_WaitTimes[i][j][k] = 0;
        }
    }
}

BattlegroundQueue::~BattlegroundQueue()
{
    m_events.KillAllEvents(false);

    m_QueuedPlayers.clear();
    for (int i = 0; i < MAX_BATTLEGROUND_BRACKETS; ++i)
    {
        for (uint32 j = 0; j < BG_QUEUE_GROUP_TYPES_COUNT; ++j)
        {
            for (GroupsQueueType::iterator itr = m_QueuedGroups[i][j].begin(); itr!= m_QueuedGroups[i][j].end(); ++itr)
                delete (*itr);
            m_QueuedGroups[i][j].clear();
        }
    }
}

/*********************************************************/
/***      BATTLEGROUND QUEUE SELECTION POOLS           ***/
/*********************************************************/

// selection pool initialization, used to clean up from prev selection
void BattlegroundQueue::SelectionPool::Init()
{
    SelectedGroups.clear();
    PlayerCount = 0;
}

// remove group info from selection pool
// returns true when we need to try to add new group to selection pool
// returns false when selection pool is ok or when we kicked smaller group than we need to kick
// sometimes it can be called on empty selection pool
bool BattlegroundQueue::SelectionPool::KickGroup(uint32 size)
{
    //find maxgroup or LAST group with size == size and kick it
    bool found = false;
    GroupsQueueType::iterator groupToKick = SelectedGroups.begin();
    for (GroupsQueueType::iterator itr = groupToKick; itr != SelectedGroups.end(); ++itr)
    {
        if (abs((int32)((*itr)->Players.size() - size)) <= 1)
        {
            groupToKick = itr;
            found = true;
        }
        else if (!found && (*itr)->Players.size() >= (*groupToKick)->Players.size())
            groupToKick = itr;
    }
    //if pool is empty, do nothing
    if (GetPlayerCount())
    {
        //update player count
        GroupQueueInfo* ginfo = (*groupToKick);
        SelectedGroups.erase(groupToKick);
        PlayerCount -= ginfo->Players.size();
        //return false if we kicked smaller group or there are enough players in selection pool
        if (ginfo->Players.size() <= size + 1)
            return false;
    }
    return true;
}

// add group to selection pool
// used when building selection pools
// returns true if we can invite more players, or when we added group to selection pool
// returns false when selection pool is full
bool BattlegroundQueue::SelectionPool::AddGroup(GroupQueueInfo* ginfo, uint32 desiredCount)
{
    //if group is larger than desired count - don't allow to add it to pool
    if (!ginfo->IsInvitedToBGInstanceGUID && desiredCount >= PlayerCount + ginfo->Players.size())
    {
        SelectedGroups.push_back(ginfo);
        // increase selected players count
        PlayerCount += ginfo->Players.size();
        return true;
    }
    if (PlayerCount < desiredCount)
        return true;
    return false;
}

/*********************************************************/
/***               BATTLEGROUND QUEUES                 ***/
/*********************************************************/

// add group or player (grp == NULL) to bg queue with the given leader and bg specifications
GroupQueueInfo* BattlegroundQueue::AddGroup(Player* p_Leader, Group* p_Group, BattlegroundTypeId p_BgTypeId, PvPDifficultyEntry const*  p_BracketEntry, uint8 p_ArenaType, bool p_IsRatedBG, bool p_IsPremade, uint32 p_ArenaRating, uint32 p_MatchmakerRating, bool p_IsSkirmish)
{
    BattlegroundBracketId l_BracketId = p_BracketEntry->GetBracketId();

    // create new ginfo
    GroupQueueInfo* l_GroupQueue            = new GroupQueueInfo;
    l_GroupQueue->BgTypeId                  = p_BgTypeId;
    l_GroupQueue->ArenaType                 = p_ArenaType;
    l_GroupQueue->IsRatedBG                 = p_IsRatedBG;
    l_GroupQueue->IsSkirmish                = p_IsSkirmish;
    l_GroupQueue->IsInvitedToBGInstanceGUID = 0;
    l_GroupQueue->JoinTime                  = getMSTime();
    l_GroupQueue->RemoveInviteTime          = 0;
    l_GroupQueue->Team                      = p_Leader->GetTeam();
    l_GroupQueue->ArenaTeamRating           = p_ArenaRating;
    l_GroupQueue->ArenaMatchmakerRating     = p_MatchmakerRating;
    l_GroupQueue->OpponentsTeamRating       = 0;
    l_GroupQueue->OpponentsMatchmakerRating = 0;
    l_GroupQueue->group                     = p_Group;

    l_GroupQueue->Players.clear();

    //compute index (if group is premade or joined a rated match) to queues
    uint32 l_Index = 0;
    if (!p_IsRatedBG && !p_IsPremade)
        l_Index += BG_TEAMS_COUNT;
    if (l_GroupQueue->Team == HORDE)
        l_Index++;

    uint32 l_LastOnlineTime = getMSTime();

    //add players from group to ginfo
    {
        if (p_Group)
        {
            for (GroupReference* l_Iterator = p_Group->GetFirstMember(); l_Iterator != NULL; l_Iterator = l_Iterator->next())
            {
                Player* l_Member = l_Iterator->getSource();
                if (!l_Member)
                    continue;

                PlayerQueueInfo& l_PlayerQueue = m_QueuedPlayers[l_Member->GetGUID()];
                l_PlayerQueue.LastOnlineTime   = l_LastOnlineTime;
                l_PlayerQueue.GroupInfo        = l_GroupQueue;

                l_GroupQueue->Players[l_Member->GetGUID()]  = &l_PlayerQueue;
            }
        }
        else
        {
            PlayerQueueInfo& l_PlayerQueue = m_QueuedPlayers[p_Leader->GetGUID()];
            l_PlayerQueue.LastOnlineTime   = l_LastOnlineTime;
            l_PlayerQueue.GroupInfo        = l_GroupQueue;

            l_GroupQueue->Players[p_Leader->GetGUID()]  = &l_PlayerQueue;
        }

        m_QueuedGroups[l_BracketId][l_Index].push_back(l_GroupQueue);
    }

    return l_GroupQueue;
}

void BattlegroundQueue::PlayerInvitedToBGUpdateAverageWaitTime(GroupQueueInfo* ginfo, BattlegroundBracketId bracket_id)
{
    uint32 timeInQueue = getMSTimeDiff(ginfo->JoinTime, getMSTime());
    uint8 team_index = BG_TEAM_ALLIANCE;                    //default set to BG_TEAM_ALLIANCE - or non rated arenas!
    if (!ginfo->ArenaType)
    {
        if (ginfo->Team == HORDE)
            team_index = BG_TEAM_HORDE;
    }
    else
    {
        if (ginfo->IsRatedBG)
            team_index = BG_TEAM_HORDE;                     //for rated arenas use BG_TEAM_HORDE
    }

    //store pointer to arrayindex of player that was added first
    uint32* lastPlayerAddedPointer = &(m_WaitTimeLastPlayer[team_index][bracket_id]);
    //remove his time from sum
    m_SumOfWaitTimes[team_index][bracket_id] -= m_WaitTimes[team_index][bracket_id][(*lastPlayerAddedPointer)];
    //set average time to new
    m_WaitTimes[team_index][bracket_id][(*lastPlayerAddedPointer)] = timeInQueue;
    //add new time to sum
    m_SumOfWaitTimes[team_index][bracket_id] += timeInQueue;
    //set index of last player added to next one
    (*lastPlayerAddedPointer)++;
    (*lastPlayerAddedPointer) %= COUNT_OF_PLAYERS_TO_AVERAGE_WAIT_TIME;
}

uint32 BattlegroundQueue::GetAverageQueueWaitTime(GroupQueueInfo* ginfo, BattlegroundBracketId bracket_id) const
{
    uint8 team_index = BG_TEAM_ALLIANCE;                    //default set to BG_TEAM_ALLIANCE - or non rated arenas!
    if (!ginfo->ArenaType)
    {
        if (ginfo->Team == HORDE)
            team_index = BG_TEAM_HORDE;
    }
    else
    {
        if (ginfo->IsRatedBG)
            team_index = BG_TEAM_HORDE;                     //for rated arenas use BG_TEAM_HORDE
    }
    //check if there is enought values(we always add values > 0)
    if (m_WaitTimes[team_index][bracket_id][COUNT_OF_PLAYERS_TO_AVERAGE_WAIT_TIME - 1])
        return (m_SumOfWaitTimes[team_index][bracket_id] / COUNT_OF_PLAYERS_TO_AVERAGE_WAIT_TIME);
    else
        //if there aren't enough values return 0 - not available
        return 0;
}

//remove player from queue and from group info, if group info is empty then remove it too
void BattlegroundQueue::RemovePlayer(uint64 guid, bool decreaseInvitedCount)
{
    //Player* player = ObjectAccessor::FindPlayer(guid);

    int32 bracket_id = -1;                                     // signed for proper for-loop finish
    QueuedPlayersMap::iterator itr;

    //remove player from map, if he's there
    itr = m_QueuedPlayers.find(guid);
    if (itr == m_QueuedPlayers.end())
        return;

    GroupQueueInfo* group = itr->second.GroupInfo;
    GroupsQueueType::iterator group_itr, group_itr_tmp;
    // mostly people with the highest levels are in battlegrounds, thats why
    // we count from MAX_BATTLEGROUND_QUEUES - 1 to 0
    // variable index removes useless searching in other team's queue
    uint32 index = (group->Team == HORDE) ? BG_TEAM_HORDE : BG_TEAM_ALLIANCE;

    for (int32 bracket_id_tmp = MAX_BATTLEGROUND_BRACKETS - 1; bracket_id_tmp >= 0 && bracket_id == -1; --bracket_id_tmp)
    {
        //we must check premade and normal team's queue - because when players from premade are joining bg,
        //they leave groupinfo so we can't use its players size to find out index
        for (uint32 j = index; j < BG_QUEUE_GROUP_TYPES_COUNT; j += BG_TEAMS_COUNT)
        {
            for (group_itr_tmp = m_QueuedGroups[bracket_id_tmp][j].begin(); group_itr_tmp != m_QueuedGroups[bracket_id_tmp][j].end(); ++group_itr_tmp)
            {
                if ((*group_itr_tmp) == group)
                {
                    bracket_id = bracket_id_tmp;
                    group_itr = group_itr_tmp;
                    //we must store index to be able to erase iterator
                    index = j;
                    break;
                }
            }
        }
    }

    //player can't be in queue without group, but just in case
    if (bracket_id == -1)
    {
        sLog->outError(LOG_FILTER_BATTLEGROUND, "BattlegroundQueue: ERROR Cannot find groupinfo for player GUID: %u", GUID_LOPART(guid));
        return;
    }
    sLog->outDebug(LOG_FILTER_BATTLEGROUND, "BattlegroundQueue: Removing player GUID %u, from bracket_id %u", GUID_LOPART(guid), (uint32)bracket_id);

    // ALL variables are correctly set
    // We can ignore leveling up in queue - it should not cause crash
    // remove player from group
    // if only one player there, remove group

    // remove player queue info from group queue info
    std::map<uint64, PlayerQueueInfo*>::iterator pitr = group->Players.find(guid);
    if (pitr != group->Players.end())
        group->Players.erase(pitr);

    // if invited to bg, and should decrease invited count, then do it
    if (decreaseInvitedCount && group->IsInvitedToBGInstanceGUID)
        if (Battleground* bg = sBattlegroundMgr->GetBattleground(group->IsInvitedToBGInstanceGUID, group->BgTypeId == BATTLEGROUND_AA ? BATTLEGROUND_TYPE_NONE : group->BgTypeId))
            bg->DecreaseInvitedCount(group->Team);

    // remove player queue info
    m_QueuedPlayers.erase(itr);


    // if player leaves queue and he is invited to rated arena match, then he have to lose
    if (group->IsInvitedToBGInstanceGUID && group->IsSkirmish && decreaseInvitedCount)
    {
        if (Player* player = ObjectAccessor::FindPlayer(guid))
        {
            // Update personal rating
            uint8 slot = Arena::GetSlotByType(group->ArenaType);
            int32 mod = Arena::GetRatingMod(player->GetArenaPersonalRating(slot), group->OpponentsMatchmakerRating, false);
            player->SetArenaPersonalRating(slot, player->GetArenaPersonalRating(slot) + mod);

            // Update matchmaker rating
            player->SetArenaMatchMakerRating(slot, player->GetArenaMatchMakerRating(slot) - 12);

            // Update personal played stats
            player->IncrementWeekGames(slot);
            player->IncrementSeasonGames(slot);
        }
    }

    // remove group queue info if needed
    if (group->Players.empty())
    {
        m_QueuedGroups[bracket_id][index].erase(group_itr);
        delete group;
    }
    // if group wasn't empty, so it wasn't deleted, and player have left a rated
    // queue -> everyone from the group should leave too
    // don't remove recursively if already invited to bg!
    else if (!group->IsInvitedToBGInstanceGUID && group->IsRatedBG)
    {
        // remove next player, this is recursive
        // first send removal information
        if (Player* plr2 = ObjectAccessor::FindPlayer(group->Players.begin()->first))
        {
            Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(group->BgTypeId);
            BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(group->BgTypeId, group->ArenaType);
            uint32 queueSlot = plr2->GetBattlegroundQueueIndex(bgQueueTypeId);
            plr2->RemoveBattlegroundQueueId(bgQueueTypeId); // must be called this way, because if you move this call to
                                                            // queue->removeplayer, it causes bugs
            WorldPacket data;
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, plr2, queueSlot, STATUS_NONE, plr2->GetBattlegroundQueueJoinTime(group->BgTypeId), 0, 0);
            plr2->GetSession()->SendPacket(&data);
        }
        // then actually delete, this may delete the group as well!
        RemovePlayer(group->Players.begin()->first, decreaseInvitedCount);
    }
}

//returns true when player pl_guid is in queue and is invited to bgInstanceGuid
bool BattlegroundQueue::IsPlayerInvited(uint64 pl_guid, const uint32 bgInstanceGuid, const uint32 removeTime)
{
    QueuedPlayersMap::const_iterator qItr = m_QueuedPlayers.find(pl_guid);
    return (qItr != m_QueuedPlayers.end()
        && qItr->second.GroupInfo->IsInvitedToBGInstanceGUID == bgInstanceGuid
        && qItr->second.GroupInfo->RemoveInviteTime == removeTime);
}

bool BattlegroundQueue::GetPlayerGroupInfoData(uint64 guid, GroupQueueInfo* ginfo)
{
    QueuedPlayersMap::const_iterator qItr = m_QueuedPlayers.find(guid);
    if (qItr == m_QueuedPlayers.end())
        return false;
    *ginfo = *(qItr->second.GroupInfo);
    return true;
}

bool BattlegroundQueue::InviteGroupToBG(GroupQueueInfo* ginfo, Battleground* bg, uint32 side)
{
    // set side if needed
    if (side)
        ginfo->Team = side;

    if (!ginfo->IsInvitedToBGInstanceGUID)
    {
        // not yet invited
        // set invitation
        ginfo->IsInvitedToBGInstanceGUID = bg->GetInstanceID();
        BattlegroundTypeId bgTypeId = bg->GetTypeID();
        BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, bg->GetArenaType(), ginfo->IsSkirmish);
        BattlegroundBracketId bracket_id = bg->GetBracketId();

        ginfo->RemoveInviteTime = getMSTime() + INVITE_ACCEPT_WAIT_TIME;

        // loop through the players
        for (std::map<uint64, PlayerQueueInfo*>::iterator itr = ginfo->Players.begin(); itr != ginfo->Players.end(); ++itr)
        {
            // get the player
            Player* player = ObjectAccessor::FindPlayer(itr->first);
            // if offline, skip him, this should not happen - player is removed from queue when he logs out
            if (!player)
                continue;

            // invite the player
            PlayerInvitedToBGUpdateAverageWaitTime(ginfo, bracket_id);
            //sBattlegroundMgr->InvitePlayer(player, bg, ginfo->Team);

            // set invited player counters
            bg->IncreaseInvitedCount(ginfo->Team);

            player->SetInviteForBattlegroundQueueType(bgQueueTypeId, ginfo->IsInvitedToBGInstanceGUID);

            // create remind invite events
            BGQueueInviteEvent* inviteEvent = new BGQueueInviteEvent(player->GetGUID(), ginfo->IsInvitedToBGInstanceGUID, bgTypeId, ginfo->ArenaType, ginfo->RemoveInviteTime);
            m_events.AddEvent(inviteEvent, m_events.CalculateTime(INVITATION_REMIND_TIME));
            // create automatic remove events
            BGQueueRemoveEvent* removeEvent = new BGQueueRemoveEvent(player->GetGUID(), ginfo->IsInvitedToBGInstanceGUID, bgTypeId, bgQueueTypeId, ginfo->RemoveInviteTime);
            m_events.AddEvent(removeEvent, m_events.CalculateTime(INVITE_ACCEPT_WAIT_TIME));

            WorldPacket data;

            uint32 queueSlot = player->GetBattlegroundQueueIndex(bgQueueTypeId);

            sLog->outDebug(LOG_FILTER_BATTLEGROUND, "Battleground: invited player %s (%u) to BG instance %u queueindex %u bgtype %u, I can't help it if they don't press the enter battle button.", player->GetName(), player->GetGUIDLow(), bg->GetInstanceID(), queueSlot, bg->GetTypeID());

            // send status packet
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, player, queueSlot, STATUS_WAIT_JOIN, INVITE_ACCEPT_WAIT_TIME, player->GetBattlegroundQueueJoinTime(bgTypeId), ginfo->ArenaType, ginfo->IsSkirmish);
            player->GetSession()->SendPacket(&data);
        }
        return true;
    }

    return false;
}

/*
This function is inviting players to already running battlegrounds
Invitation type is based on config file
large groups are disadvantageous, because they will be kicked first if invitation type = 1
*/
void BattlegroundQueue::FillPlayersToBG(Battleground* bg, BattlegroundBracketId bracket_id)
{
    int32 hordeFree = bg->GetFreeSlotsForTeam(HORDE);
    int32 aliFree   = bg->GetFreeSlotsForTeam(ALLIANCE);

    //iterator for iterating through bg queue
    GroupsQueueType::const_iterator Ali_itr = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE].begin();
    //count of groups in queue - used to stop cycles
    uint32 aliCount = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE].size();
    //index to queue which group is current
    uint32 aliIndex = 0;
    for (; aliIndex < aliCount && m_SelectionPools[BG_TEAM_ALLIANCE].AddGroup((*Ali_itr), aliFree); aliIndex++)
        ++Ali_itr;
    //the same thing for horde
    GroupsQueueType::const_iterator Horde_itr = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_HORDE].begin();
    uint32 hordeCount = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_HORDE].size();
    uint32 hordeIndex = 0;
    for (; hordeIndex < hordeCount && m_SelectionPools[BG_TEAM_HORDE].AddGroup((*Horde_itr), hordeFree); hordeIndex++)
        ++Horde_itr;

    //if ofc like BG queue invitation is set in config, then we are happy
    if (sWorld->getIntConfig(CONFIG_BATTLEGROUND_INVITATION_TYPE) == 0)
        return;

    /*
    if we reached this code, then we have to solve NP - complete problem called Subset sum problem
    So one solution is to check all possible invitation subgroups, or we can use these conditions:
    1. Last time when BattlegroundQueue::Update was executed we invited all possible players - so there is only small possibility
        that we will invite now whole queue, because only 1 change has been made to queues from the last BattlegroundQueue::Update call
    2. Other thing we should consider is group order in queue
    */

    // At first we need to compare free space in bg and our selection pool
    int32 diffAli   = aliFree   - int32(m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount());
    int32 diffHorde = hordeFree - int32(m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount());
    while (abs(diffAli - diffHorde) > 1 && (m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() > 0 || m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount() > 0))
    {
        //each cycle execution we need to kick at least 1 group
        if (diffAli < diffHorde)
        {
            //kick alliance group, add to pool new group if needed
            if (m_SelectionPools[BG_TEAM_ALLIANCE].KickGroup(diffHorde - diffAli))
            {
                for (; aliIndex < aliCount && m_SelectionPools[BG_TEAM_ALLIANCE].AddGroup((*Ali_itr), (aliFree >= diffHorde) ? aliFree - diffHorde : 0); aliIndex++)
                    ++Ali_itr;
            }
            //if ali selection is already empty, then kick horde group, but if there are less horde than ali in bg - break;
            if (!m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount())
            {
                if (aliFree <= diffHorde + 1)
                    break;
                m_SelectionPools[BG_TEAM_HORDE].KickGroup(diffHorde - diffAli);
            }
        }
        else
        {
            //kick horde group, add to pool new group if needed
            if (m_SelectionPools[BG_TEAM_HORDE].KickGroup(diffAli - diffHorde))
            {
                for (; hordeIndex < hordeCount && m_SelectionPools[BG_TEAM_HORDE].AddGroup((*Horde_itr), (hordeFree >= diffAli) ? hordeFree - diffAli : 0); hordeIndex++)
                    ++Horde_itr;
            }
            if (!m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount())
            {
                if (hordeFree <= diffAli + 1)
                    break;
                m_SelectionPools[BG_TEAM_ALLIANCE].KickGroup(diffAli - diffHorde);
            }
        }
        //count diffs after small update
        diffAli   = aliFree   - int32(m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount());
        diffHorde = hordeFree - int32(m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount());
    }
}

// this method checks if premade versus premade battleground is possible
// then after 30 mins (default) in queue it moves premade group to normal queue
// it tries to invite as much players as it can - to MaxPlayersPerTeam, be       cause premade groups have more than MinPlayersPerTeam players
bool BattlegroundQueue::CheckPremadeMatch(BattlegroundBracketId bracket_id, uint32 MinPlayersPerTeam, uint32 MaxPlayersPerTeam)
{
    //check match
    if (!m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].empty() && !m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].empty())
    {
        //start premade match
        //if groups aren't invited
        GroupsQueueType::const_iterator ali_group, horde_group;
        for (ali_group = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].begin(); ali_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].end(); ++ali_group)
            if (!(*ali_group)->IsInvitedToBGInstanceGUID)
                break;
        for (horde_group = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].begin(); horde_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].end(); ++horde_group)
            if (!(*horde_group)->IsInvitedToBGInstanceGUID)
                break;

        if (ali_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].end() && horde_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].end())
        {
            m_SelectionPools[BG_TEAM_ALLIANCE].AddGroup((*ali_group), MaxPlayersPerTeam);
            m_SelectionPools[BG_TEAM_HORDE].AddGroup((*horde_group), MaxPlayersPerTeam);
            //add groups/players from normal queue to size of bigger group
            uint32 maxPlayers = std::min(m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount(), m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount());
            GroupsQueueType::const_iterator itr;
            for (uint32 i = 0; i < BG_TEAMS_COUNT; i++)
            {
                for (itr = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].begin(); itr != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].end(); ++itr)
                {
                    //if itr can join BG and player count is less that maxPlayers, then add group to selectionpool
                    if (!(*itr)->IsInvitedToBGInstanceGUID && !m_SelectionPools[i].AddGroup((*itr), maxPlayers))
                        break;
                }
            }
            //premade selection pools are set
            return true;
        }
    }
    // now check if we can move group from Premade queue to normal queue (timer has expired) or group size lowered!!
    // this could be 2 cycles but i'm checking only first team in queue - it can cause problem -
    // if first is invited to BG and seconds timer expired, but we can ignore it, because players have only 80 seconds to click to enter bg
    // and when they click or after 80 seconds the queue info is removed from queue
    uint32 l_TimeNow     = getMSTime();
    uint32 l_ExpireTime = sWorld->getIntConfig(CONFIG_BATTLEGROUND_PREMADE_GROUP_WAIT_FOR_MATCH);

    for (uint32 i = 0; i < BG_TEAMS_COUNT; i++)
    {
        if (!m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE + i].empty())
        {
            GroupsQueueType::iterator itr = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE + i].begin();
            if (!(*itr)->IsInvitedToBGInstanceGUID && (((*itr)->JoinTime + l_ExpireTime) < l_TimeNow || (*itr)->Players.size() < MinPlayersPerTeam))
            {
                //we must insert group to normal queue and erase pointer from premade queue
                m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].push_front((*itr));
                m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE + i].erase(itr);
            }
        }
    }
    //selection pools are not set
    return false;
}

// this method tries to create battleground or arena with MinPlayersPerTeam against MinPlayersPerTeam
bool BattlegroundQueue::CheckNormalMatch(Battleground* bg_template, BattlegroundBracketId bracket_id, uint32 minPlayers, uint32 maxPlayers)
{
    GroupsQueueType::const_iterator itr_team[BG_TEAMS_COUNT];
    for (uint32 i = 0; i < BG_TEAMS_COUNT; i++)
    {
        itr_team[i] = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].begin();
        for (; itr_team[i] != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].end(); ++(itr_team[i]))
        {
            if (!(*(itr_team[i]))->IsInvitedToBGInstanceGUID)
            {
                m_SelectionPools[i].AddGroup(*(itr_team[i]), maxPlayers);
                if (m_SelectionPools[i].GetPlayerCount() >= minPlayers)
                    break;
            }
        }
    }
    //try to invite same number of players - this cycle may cause longer wait time even if there are enough players in queue, but we want ballanced bg
    uint32 j = BG_TEAM_ALLIANCE;
    if (m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() < m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount())
        j = BG_TEAM_HORDE;
    if (sWorld->getIntConfig(CONFIG_BATTLEGROUND_INVITATION_TYPE) != 0
        && m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() >= minPlayers && m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount() >= minPlayers)
    {
        //we will try to invite more groups to team with less players indexed by j
        ++(itr_team[j]);                                         //this will not cause a crash, because for cycle above reached break;
        for (; itr_team[j] != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + j].end(); ++(itr_team[j]))
        {
            if (!(*(itr_team[j]))->IsInvitedToBGInstanceGUID)
                if (!m_SelectionPools[j].AddGroup(*(itr_team[j]), m_SelectionPools[(j + 1) % BG_TEAMS_COUNT].GetPlayerCount()))
                    break;
        }
        // do not allow to start bg with more than 2 players more on 1 faction
        if (abs((int32)(m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() - m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount())) > 2)
            return false;
    }
    //allow 1v0 if debug bg
    if (sBattlegroundMgr->isTesting() && bg_template->isBattleground() && (m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount() || m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount()))
        return true;
    //return true if there are enough players in selection pools - enable to work .debug bg command correctly
    return m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount() >= minPlayers && m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() >= minPlayers;
}

// this method will check if we can invite players to same faction skirmish match
bool BattlegroundQueue::CheckSkirmishForSameFaction(BattlegroundBracketId bracket_id, uint32 minPlayersPerTeam)
{
    if (m_SelectionPools[BG_TEAM_ALLIANCE].GetPlayerCount() < minPlayersPerTeam && m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() < minPlayersPerTeam)
        return false;

    uint32 teamIndex = BG_TEAM_ALLIANCE;
    uint32 otherTeam = BG_TEAM_HORDE;
    uint32 otherTeamId = HORDE;
    if (m_SelectionPools[BG_TEAM_HORDE].GetPlayerCount() == minPlayersPerTeam)
    {
        teamIndex = BG_TEAM_HORDE;
        otherTeam = BG_TEAM_ALLIANCE;
        otherTeamId = ALLIANCE;
    }
    //clear other team's selection
    m_SelectionPools[otherTeam].Init();
    //store last ginfo pointer
    GroupQueueInfo* ginfo = m_SelectionPools[teamIndex].SelectedGroups.back();
    //set itr_team to group that was added to selection pool latest
    GroupsQueueType::iterator itr_team = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].begin();
    for (; itr_team != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].end(); ++itr_team)
        if (ginfo == *itr_team)
            break;

    if (itr_team == m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].end())
        return false;

    GroupsQueueType::iterator itr_team2 = itr_team;
    ++itr_team2;
    //invite players to other selection pool
    for (; itr_team2 != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].end(); ++itr_team2)
    {
        //if selection pool is full then break;
        if (!(*itr_team2)->IsInvitedToBGInstanceGUID && !m_SelectionPools[otherTeam].AddGroup(*itr_team2, minPlayersPerTeam))
            break;
    }
    if (m_SelectionPools[otherTeam].GetPlayerCount() != minPlayersPerTeam)
        return false;

    //here we have correct 2 selections and we need to change one teams team and move selection pool teams to other team's queue
    for (GroupsQueueType::iterator itr = m_SelectionPools[otherTeam].SelectedGroups.begin(); itr != m_SelectionPools[otherTeam].SelectedGroups.end(); ++itr)
    {
        //set correct team
        (*itr)->Team = otherTeamId;
        //add team to other queue
        m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + otherTeam].push_front(*itr);
        //remove team from old queue
        GroupsQueueType::iterator itr2 = itr_team;
        ++itr2;
        for (; itr2 != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].end(); ++itr2)
        {
            if (*itr2 == *itr)
            {
                m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].erase(itr2);
                break;
            }
        }
    }
    return true;
}

void BattlegroundQueue::UpdateEvents(uint32 diff)
{
    m_events.Update(diff);
}

/*
this method is called when group is inserted, or player / group is removed from BG Queue - there is only one player's status changed, so we don't use while (true) cycles to invite whole queue
it must be called after fully adding the members of a group to ensure group joining
should be called from Battleground::RemovePlayer function in some cases
*/
void BattlegroundQueue::BattlegroundQueueUpdate(BattlegroundTypeId p_BgTypeId, BattlegroundBracketId p_BracketId, uint8 p_ArenaType, uint32 p_ArenaRating, bool p_IsSkirmish)
{
    //if no players in queue - do nothing
    if (m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_ALLIANCE].empty() &&
        m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_HORDE].empty() &&
        m_QueuedGroups[p_BracketId][BG_QUEUE_NORMAL_ALLIANCE].empty() &&
        m_QueuedGroups[p_BracketId][BG_QUEUE_NORMAL_HORDE].empty())
        return;

    //battleground with free slot for player should be always in the beggining of the queue
    // maybe it would be better to create bgfreeslotqueue for each bracket_id
    BGFreeSlotQueueType::iterator l_Itr, l_Next;
    for (l_Itr = sBattlegroundMgr->BGFreeSlotQueue[p_BgTypeId].begin(); l_Itr != sBattlegroundMgr->BGFreeSlotQueue[p_BgTypeId].end(); l_Itr = l_Next)
    {
        l_Next = l_Itr;
        ++l_Next;
        // DO NOT allow queue manager to invite new player to arena
        if ((*l_Itr)->isBattleground() && (*l_Itr)->GetTypeID() == p_BgTypeId && (*l_Itr)->GetBracketId() == p_BracketId &&
            (*l_Itr)->GetStatus() > STATUS_WAIT_QUEUE && (*l_Itr)->GetStatus() < STATUS_WAIT_LEAVE)
        {
            Battleground* l_Battleground = *l_Itr; // we have to store battleground pointer here, because when battleground is full, it is removed from free queue (not yet implemented!!)
            // and iterator is invalid

            // clear selection pools
            m_SelectionPools[BG_TEAM_ALLIANCE].Init();
            m_SelectionPools[BG_TEAM_HORDE].Init();

            // call a function that does the job for us
            FillPlayersToBG(l_Battleground, p_BracketId);

            // now everything is set, invite players
            for (GroupsQueueType::const_iterator GroupQueueItr = m_SelectionPools[BG_TEAM_ALLIANCE].SelectedGroups.begin(); GroupQueueItr != m_SelectionPools[BG_TEAM_ALLIANCE].SelectedGroups.end(); ++GroupQueueItr)
                InviteGroupToBG((*GroupQueueItr), l_Battleground, (*GroupQueueItr)->Team);
            for (GroupsQueueType::const_iterator GroupQueueItr = m_SelectionPools[BG_TEAM_HORDE].SelectedGroups.begin(); GroupQueueItr != m_SelectionPools[BG_TEAM_HORDE].SelectedGroups.end(); ++GroupQueueItr)
                InviteGroupToBG((*GroupQueueItr), l_Battleground, (*GroupQueueItr)->Team);

            if (!l_Battleground->HasFreeSlots())
            {
                // remove BG from BGFreeSlotQueue
                l_Battleground->RemoveFromBGFreeSlotQueue();
            }
        }
    }

    // finished iterating through the bgs with free slots, maybe we need to create a new bg

    Battleground* l_BGTemplate = sBattlegroundMgr->GetBattlegroundTemplate(p_BgTypeId);
    if (!l_BGTemplate)
    {
        sLog->outError(LOG_FILTER_BATTLEGROUND, "Battleground: Update: bg template not found for %u", p_BgTypeId);
        return;
    }

    PvPDifficultyEntry const* l_BracketEntry = GetBattlegroundBracketById(l_BGTemplate->GetMapId(), p_BracketId);
    if (!l_BracketEntry)
    {
        sLog->outError(LOG_FILTER_BATTLEGROUND, "Battleground: Update: bg bracket entry not found for map %u bracket id %u", l_BGTemplate->GetMapId(), p_BracketId);
        return;
    }

    /// - get the min. players per team, properly for larger arenas as well. (must have full teams for arena matches!)
    uint32 l_MinPlayersPerTeam = l_BGTemplate->GetMinPlayersPerTeam();
    uint32 l_MaxPlayersPerTeam = l_BGTemplate->GetMaxPlayersPerTeam();

    if (sBattlegroundMgr->isTesting())
        l_MinPlayersPerTeam = 1;

    if (l_BGTemplate->isArena())
    {
        if (sBattlegroundMgr->isArenaTesting())
        {
            l_MaxPlayersPerTeam = 1;
            l_MinPlayersPerTeam = 1;
        }
        else
        {
            /// - this switch can be much shorter
            l_MaxPlayersPerTeam = p_ArenaType;
            l_MinPlayersPerTeam = p_ArenaType;
        }
    }

    m_SelectionPools[BG_TEAM_ALLIANCE].Init();
    m_SelectionPools[BG_TEAM_HORDE].Init();

    if (l_BGTemplate->isBattleground() && !l_BGTemplate->IsRatedBG())
    {
        /// - check if there is premade against premade match
        if (CheckPremadeMatch(p_BracketId, l_MinPlayersPerTeam, l_MaxPlayersPerTeam))
        {
            /// - create new battleground
            Battleground* l_Battleground2 = sBattlegroundMgr->CreateNewBattleground(p_BgTypeId, l_BracketEntry, 0);
            if (!l_Battleground2)
            {
                sLog->outError(LOG_FILTER_BATTLEGROUND, "BattlegroundQueue::Update - Cannot create battleground: %u", p_BgTypeId);
                return;
            }

            /// - invite those selection pools
            for (uint32 i = 0; i < BG_TEAMS_COUNT; i++)
            {
                for (GroupsQueueType::const_iterator l_GroupQueueItr = m_SelectionPools[BG_TEAM_ALLIANCE + i].SelectedGroups.begin(); l_GroupQueueItr != m_SelectionPools[BG_TEAM_ALLIANCE + i].SelectedGroups.end(); ++l_GroupQueueItr)
                    InviteGroupToBG((*l_GroupQueueItr), l_Battleground2, (*l_GroupQueueItr)->Team);
            }

            /// - start bg
            l_Battleground2->StartBattleground();

            /// - clear structures
            m_SelectionPools[BG_TEAM_ALLIANCE].Init();
            m_SelectionPools[BG_TEAM_HORDE].Init();
        }
    }

    // now check if there are in queues enough players to start new game of (normal battleground, or non-rated arena)
    if (!l_BGTemplate->IsRatedBG() && !(l_BGTemplate->isArena() && !p_IsSkirmish))
    {
        // if there are enough players in pools, start new battleground or non rated arena
        if (CheckNormalMatch(l_BGTemplate, p_BracketId, l_MinPlayersPerTeam, l_MaxPlayersPerTeam)
            || (l_BGTemplate->isArena() && CheckSkirmishForSameFaction(p_BracketId, l_MinPlayersPerTeam))
            || (l_BGTemplate->isArena() && CheckPremadeMatch(p_BracketId, l_MinPlayersPerTeam, l_MinPlayersPerTeam)))
        {
            // we successfully created a pool
            Battleground* l_Battleground2 = sBattlegroundMgr->CreateNewBattleground(p_BgTypeId, l_BracketEntry, p_ArenaType, l_BGTemplate->isArena());
            if (!l_Battleground2)
            {
                sLog->outError(LOG_FILTER_BATTLEGROUND, "BattlegroundQueue::Update - Cannot create battleground: %u", p_BgTypeId);
                return;
            }

            // invite those selection pools
            for (uint32 l_I = 0; l_I < BG_TEAMS_COUNT; l_I++)
            {
                for (GroupsQueueType::const_iterator l_GroupQueueItr = m_SelectionPools[BG_TEAM_ALLIANCE + l_I].SelectedGroups.begin(); l_GroupQueueItr != m_SelectionPools[BG_TEAM_ALLIANCE + l_I].SelectedGroups.end(); ++l_GroupQueueItr)
                    InviteGroupToBG((*l_GroupQueueItr), l_Battleground2, (*l_GroupQueueItr)->Team);
            }

            // start bg
            l_Battleground2->StartBattleground();
        }
    }
    else if (l_BGTemplate->isArena())
    {
        // found out the minimum and maximum ratings the newly added team should battle against
        // arenaRating is the rating of the latest joined team, or 0
        // 0 is on (automatic update call) and we must set it to team's with longest wait time
        uint32 l_MmrMaxDiff = 0;
        if (!p_ArenaRating)
        {
            GroupQueueInfo* l_Front1 = nullptr;
            GroupQueueInfo* l_Front2 = nullptr;

            if (!m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_ALLIANCE].empty())
            {
                l_Front1 = m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_ALLIANCE].front();
                p_ArenaRating = l_Front1->ArenaMatchmakerRating;

                float l_MmrSteps = floor(float((getMSTime() - l_Front1->JoinTime) / 60000)); // every 1 minute
                l_MmrMaxDiff = l_MmrSteps * 150; // increase range up to 150
            }
            if (!m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_HORDE].empty())
            {
                l_Front2 = m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_HORDE].front();
                p_ArenaRating = l_Front2->ArenaMatchmakerRating;

                float l_MmrSteps = floor(float((getMSTime() - l_Front2->JoinTime) / 60000)); // every 1 minute
                l_MmrMaxDiff = l_MmrSteps * 150; // increase range up to 150
            }
            if (l_Front1 && l_Front2)
            {
                if (l_Front1->JoinTime < l_Front2->JoinTime)
                {
                    p_ArenaRating = l_Front1->ArenaMatchmakerRating;
                    float l_MmrSteps = floor(float((getMSTime() - l_Front1->JoinTime) / 60000)); // every 1 minute
                    l_MmrMaxDiff = l_MmrSteps * 150; // increase range up to 150
                }
            }
            else if (!l_Front1 && !l_Front2)
                return; //queues are empty
        }

        //set rating range
        uint32 l_ArenaMinRating = (p_ArenaRating <= sBattlegroundMgr->GetMaxRatingDifference()) ? 0 : p_ArenaRating - sBattlegroundMgr->GetMaxRatingDifference();
        uint32 l_ArenaMaxRating = p_ArenaRating + sBattlegroundMgr->GetMaxRatingDifference();
        // if max rating difference is set and the time past since server startup is greater than the rating discard time
        // (after what time the ratings aren't taken into account when making teams) then
        // the discard time is current_time - time_to_discard, teams that joined after that, will have their ratings taken into account
        // else leave the discard time on 0, this way all ratings will be discarded
        uint32 l_DiscardTime = getMSTime() - sBattlegroundMgr->GetRatingDiscardTimer();

        if (l_MmrMaxDiff > 0)
        {
            l_ArenaMinRating = (l_MmrMaxDiff < l_ArenaMinRating) ? l_ArenaMinRating - l_MmrMaxDiff : 0;
            l_ArenaMaxRating = l_MmrMaxDiff + l_ArenaMaxRating;
        }

        // we need to find 2 teams which will play next game
        GroupsQueueType::iterator l_TeamsIterator[BG_TEAMS_COUNT];
        uint8 l_Found = 0;
        uint8 l_Team = 0;

        for (uint8 l_I = BG_QUEUE_PREMADE_ALLIANCE; l_I < BG_QUEUE_NORMAL_ALLIANCE; l_I++)
        {
            // take the group that joined first
            GroupsQueueType::iterator l_Iterator2 = m_QueuedGroups[p_BracketId][l_I].begin();
            for (; l_Iterator2 != m_QueuedGroups[p_BracketId][l_I].end(); ++l_Iterator2)
            {
                // if group match conditions, then add it to pool
                if (!(*l_Iterator2)->IsInvitedToBGInstanceGUID
                    && (((*l_Iterator2)->ArenaMatchmakerRating >= l_ArenaMinRating && (*l_Iterator2)->ArenaMatchmakerRating <= l_ArenaMaxRating)
                        || (*l_Iterator2)->JoinTime < l_DiscardTime))
                {
                    l_TeamsIterator[l_Found++] = l_Iterator2;
                    l_Team = l_I;
                    break;
                }
            }
        }

        if (!l_Found)
            return;

        if (l_Found == 1)
        {
            for (GroupsQueueType::iterator l_Iterator3 = l_TeamsIterator[0]; l_Iterator3 != m_QueuedGroups[p_BracketId][l_Team].end(); ++l_Iterator3)
            {
                if (!(*l_Iterator3)->IsInvitedToBGInstanceGUID
                    && (((*l_Iterator3)->ArenaMatchmakerRating >= l_ArenaMinRating && (*l_Iterator3)->ArenaMatchmakerRating <= l_ArenaMaxRating)
                        || (*l_Iterator3)->JoinTime < l_DiscardTime)
                    && (*l_TeamsIterator[0])->group != (*l_Iterator3)->group)
                {
                    l_TeamsIterator[l_Found++] = l_Iterator3;
                    break;
                }
            }
        }

        //if we have 2 teams, then start new arena and invite players!
        if (l_Found == 2)
        {
            GroupQueueInfo* l_AllianceTeam = *l_TeamsIterator[BG_TEAM_ALLIANCE];
            GroupQueueInfo* l_HordeTeam    = *l_TeamsIterator[BG_TEAM_HORDE];

            Battleground*   l_Arena        = sBattlegroundMgr->CreateNewBattleground(p_BgTypeId, l_BracketEntry, p_ArenaType);
            if (!l_Arena)
            {
                sLog->outError(LOG_FILTER_BATTLEGROUND, "BattlegroundQueue::Update couldn't create arena instance for rated arena match!");
                return;
            }

            l_AllianceTeam->OpponentsTeamRating = l_HordeTeam->ArenaTeamRating;
            l_HordeTeam->OpponentsTeamRating = l_AllianceTeam->ArenaTeamRating;
            l_AllianceTeam->OpponentsMatchmakerRating = l_HordeTeam->ArenaMatchmakerRating;
            l_HordeTeam->OpponentsMatchmakerRating = l_AllianceTeam->ArenaMatchmakerRating;

            // now we must move team if we changed its faction to another faction queue, because then we will spam log by errors in Queue::RemovePlayer
            if (l_AllianceTeam->Team != ALLIANCE)
            {
                m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_ALLIANCE].push_front(l_AllianceTeam);
                m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_HORDE].erase(l_TeamsIterator[BG_TEAM_ALLIANCE]);
            }
            if (l_HordeTeam->Team != HORDE)
            {
                m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_HORDE].push_front(l_HordeTeam);
                m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_ALLIANCE].erase(l_TeamsIterator[BG_TEAM_HORDE]);
            }

            InviteGroupToBG(l_AllianceTeam, l_Arena, ALLIANCE);
            InviteGroupToBG(l_HordeTeam, l_Arena, HORDE);

            sLog->outDebug(LOG_FILTER_BATTLEGROUND, "Starting rated arena match!");
            l_Arena->StartBattleground();
        }
    }
    else if (l_BGTemplate->IsRatedBG())
    {
        // found out the minimum and maximum ratings the newly added team should battle against
        // arenaRating is the rating of the latest joined team, or 0
        // 0 is on (automatic update call) and we must set it to team's with longest wait time
        if (!p_ArenaRating)
        {
            GroupQueueInfo* l_Front1 = nullptr;
            GroupQueueInfo* l_Front2 = nullptr;

            if (!m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_ALLIANCE].empty())
            {
                l_Front1 = m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_ALLIANCE].front();
                p_ArenaRating = l_Front1->ArenaMatchmakerRating;
            }
            if (!m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_HORDE].empty())
            {
                l_Front2 = m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_HORDE].front();
                p_ArenaRating = l_Front2->ArenaMatchmakerRating;
            }
            if (l_Front1 && l_Front2)
            {
                if (l_Front1->JoinTime < l_Front2->JoinTime)
                    p_ArenaRating = l_Front1->ArenaMatchmakerRating;
            }
            else if (!l_Front1 && !l_Front2)
                return; //queues are empty
        }

        //set rating range
        uint32 l_ArenaMinRating = (p_ArenaRating <= sBattlegroundMgr->GetMaxRatingDifference()) ? 0 : p_ArenaRating - sBattlegroundMgr->GetMaxRatingDifference();
        uint32 l_ArenaMaxRating = p_ArenaRating + sBattlegroundMgr->GetMaxRatingDifference();

        // if max rating difference is set and the time past since server startup is greater than the rating discard time
        // (after what time the ratings aren't taken into account when making teams) then
        // the discard time is current_time - time_to_discard, teams that joined after that, will have their ratings taken into account
        // else leave the discard time on 0, this way all ratings will be discarded
        uint32 l_DiscardTime = getMSTime() - sBattlegroundMgr->GetRatingDiscardTimer();

        // we need to find 2 teams which will play next game
        GroupsQueueType::iterator l_TeamsIterator[BG_TEAMS_COUNT];
        uint8 l_Found = 0;
        uint8 l_Team  = 0;

        for (uint8 i = BG_QUEUE_PREMADE_ALLIANCE; i < BG_QUEUE_NORMAL_ALLIANCE; i++)
        {
            // take the group that joined first
            GroupsQueueType::iterator itr2 = m_QueuedGroups[p_BracketId][i].begin();
            for (; itr2 != m_QueuedGroups[p_BracketId][i].end(); ++itr2)
            {
                // if group match conditions, then add it to pool
                if (!(*itr2)->IsInvitedToBGInstanceGUID
                    && (((*itr2)->ArenaMatchmakerRating >= l_ArenaMinRating && (*itr2)->ArenaMatchmakerRating <= l_ArenaMaxRating)
                        || (*itr2)->JoinTime < l_DiscardTime))
                {
                    l_TeamsIterator[l_Found++] = itr2;
                    l_Team = i;
                    break;
                }
            }
        }

        if (!l_Found)
            return;

        if (l_Found == 1)
        {
            for (GroupsQueueType::iterator itr3 = l_TeamsIterator[0]; itr3 != m_QueuedGroups[p_BracketId][l_Team].end(); ++itr3)
            {
                if (!(*itr3)->IsInvitedToBGInstanceGUID
                    && (((*itr3)->ArenaMatchmakerRating >= l_ArenaMinRating && (*itr3)->ArenaMatchmakerRating <= l_ArenaMaxRating)
                        || (*itr3)->JoinTime < l_DiscardTime)
                    && (*l_TeamsIterator[0])->group != (*itr3)->group)
                {
                    l_TeamsIterator[l_Found++] = itr3;
                    break;
                }
            }
        }

        //if we have 2 teams, then start new rated bg and invite players!
        if (l_Found == 2)
        {
            GroupQueueInfo* l_AllianceTeam = *l_TeamsIterator[BG_TEAM_ALLIANCE];
            GroupQueueInfo* l_HordeTeam    = *l_TeamsIterator[BG_TEAM_HORDE];

            Battleground* l_RattedBattleground = sBattlegroundMgr->CreateNewBattleground(p_BgTypeId, l_BracketEntry, 0);
            if (!l_RattedBattleground)
            {
                sLog->outError(LOG_FILTER_BATTLEGROUND, "BattlegroundQueue::Update couldn't create rated bg instance for rated bg match!");
                return;
            }

            l_AllianceTeam->OpponentsTeamRating       = l_HordeTeam->ArenaTeamRating;
            l_HordeTeam->OpponentsTeamRating          = l_AllianceTeam->ArenaTeamRating;
            l_AllianceTeam->OpponentsMatchmakerRating = l_HordeTeam->ArenaMatchmakerRating;
            l_HordeTeam->OpponentsMatchmakerRating    = l_AllianceTeam->ArenaMatchmakerRating;

            // now we must move team if we changed its faction to another faction queue, because then we will spam log by errors in Queue::RemovePlayer
            if (l_AllianceTeam->Team != ALLIANCE)
            {
                m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_ALLIANCE].push_front(l_AllianceTeam);
                m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_HORDE].erase(l_TeamsIterator[BG_TEAM_ALLIANCE]);
            }
            if (l_HordeTeam->Team != HORDE)
            {
                m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_HORDE].push_front(l_HordeTeam);
                m_QueuedGroups[p_BracketId][BG_QUEUE_PREMADE_ALLIANCE].erase(l_TeamsIterator[BG_TEAM_HORDE]);
            }

            InviteGroupToBG(l_AllianceTeam, l_RattedBattleground, ALLIANCE);
            InviteGroupToBG(l_HordeTeam, l_RattedBattleground, HORDE);

            l_RattedBattleground->StartBattleground();
        }
    }
}

/*********************************************************/
/***            BATTLEGROUND QUEUE EVENTS              ***/
/*********************************************************/

bool BGQueueInviteEvent::Execute(uint64 /*e_time*/, uint32 /*p_time*/)
{
    Player* player = ObjectAccessor::FindPlayer(m_PlayerGuid);
    // player logged off (we should do nothing, he is correctly removed from queue in another procedure)
    if (!player)
        return true;

    Battleground* bg = sBattlegroundMgr->GetBattleground(m_BgInstanceGUID, m_BgTypeId);
    //if battleground ended and its instance deleted - do nothing
    if (!bg)
        return true;

    BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType(), bg->IsSkirmish());
    uint32 queueSlot = player->GetBattlegroundQueueIndex(bgQueueTypeId);
    if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES)         // player is in queue or in battleground
    {
        // check if player is invited to this bg
        BattlegroundQueue &bgQueue = sBattlegroundMgr->m_BattlegroundQueues[bgQueueTypeId];
        if (bgQueue.IsPlayerInvited(m_PlayerGuid, m_BgInstanceGUID, m_RemoveTime))
        {
            WorldPacket data;
            //we must send remaining time in queue
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, player, queueSlot, STATUS_WAIT_JOIN, INVITE_ACCEPT_WAIT_TIME - INVITATION_REMIND_TIME, player->GetBattlegroundQueueJoinTime(m_BgTypeId), m_ArenaType);
            player->GetSession()->SendPacket(&data);
        }
    }
    return true;                                            //event will be deleted
}

void BGQueueInviteEvent::Abort(uint64 /*e_time*/)
{
    //do nothing
}

/*
    this event has many possibilities when it is executed:
    1. player is in battleground (he clicked enter on invitation window)
    2. player left battleground queue and he isn't there any more
    3. player left battleground queue and he joined it again and IsInvitedToBGInstanceGUID = 0
    4. player left queue and he joined again and he has been invited to same battleground again -> we should not remove him from queue yet
    5. player is invited to bg and he didn't choose what to do and timer expired - only in this condition we should call queue::RemovePlayer
    we must remove player in the 5. case even if battleground object doesn't exist!
*/
bool BGQueueRemoveEvent::Execute(uint64 /*e_time*/, uint32 /*p_time*/)
{
    Player* player = ObjectAccessor::FindPlayer(m_PlayerGuid);
    if (!player)
        // player logged off (we should do nothing, he is correctly removed from queue in another procedure)
        return true;

    Battleground* bg = sBattlegroundMgr->GetBattleground(m_BgInstanceGUID, m_BgTypeId);
    //battleground can be deleted already when we are removing queue info
    //bg pointer can be NULL! so use it carefully!

    uint32 queueSlot = player->GetBattlegroundQueueIndex(m_BgQueueTypeId);
    if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES)         // player is in queue, or in Battleground
    {
        // check if player is in queue for this BG and if we are removing his invite event
        BattlegroundQueue &bgQueue = sBattlegroundMgr->m_BattlegroundQueues[m_BgQueueTypeId];
        if (bgQueue.IsPlayerInvited(m_PlayerGuid, m_BgInstanceGUID, m_RemoveTime))
        {
            sLog->outDebug(LOG_FILTER_BATTLEGROUND, "Battleground: removing player %u from bg queue for instance %u because of not pressing enter battle in time.", player->GetGUIDLow(), m_BgInstanceGUID);

            player->RemoveBattlegroundQueueId(m_BgQueueTypeId);
            bgQueue.RemovePlayer(m_PlayerGuid, true);
            //update queues if battleground isn't ended
            if (bg && bg->isBattleground() && bg->GetStatus() != STATUS_WAIT_LEAVE)
                sBattlegroundMgr->ScheduleQueueUpdate(0, 0, m_BgQueueTypeId, m_BgTypeId, bg->GetBracketId());

            WorldPacket data;
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, player, queueSlot, STATUS_NONE, player->GetBattlegroundQueueJoinTime(m_BgTypeId), 0, 0);
            player->GetSession()->SendPacket(&data);
        }
    }

    //event will be deleted
    return true;
}

void BGQueueRemoveEvent::Abort(uint64 /*e_time*/)
{
    //do nothing
}
