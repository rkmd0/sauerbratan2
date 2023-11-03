#include "game.h"
#include "weaponstats.h"

namespace game
{
    bool intermission = false;
    int maptime = 0, maprealtime = 0, maplimit = -1;
    int respawnent = -1;
    int lasthit = 0, lastspawnattempt = 0;

    int following = -1, followdir = 0;

    fpsent *player1 = NULL;         // our client
    vector<fpsent *> players;       // other clients
    int savedammo[NUMGUNS];

    bool clientoption(const char *arg) { return false; }

    void taunt()
    {
        if(player1->state!=CS_ALIVE || player1->physstate<PHYS_SLOPE) return;
        if(lastmillis-player1->lasttaunt<1000) return;
        player1->lasttaunt = lastmillis;
        addmsg(N_TAUNT, "rc", player1);
    }
    COMMAND(taunt, "");

    ICOMMAND(getfollow, "", (),
    {
        fpsent *f = followingplayer();
        intret(f ? f->clientnum : -1);
    });

	void follow(char *arg)
    {
        if(arg[0] ? player1->state==CS_SPECTATOR : following>=0)
        {
            int ofollowing = following;
            following = arg[0] ? parseplayer(arg) : -1;
            if(following==player1->clientnum) following = -1;
            followdir = 0;
            if(following!=ofollowing) clearfragmessages();
            conoutf("follow %s", following>=0 ? "on" : "off");
        }
	}
    COMMAND(follow, "s");

    void nextfollow(int dir)
    {
        if(player1->state!=CS_SPECTATOR || clients.empty())
        {
            stopfollowing();
            return;
        }
        int cur = following >= 0 ? following : (dir < 0 ? clients.length() - 1 : 0);
        loopv(clients)
        {
            cur = (cur + dir + clients.length()) % clients.length();
            if(clients[cur] && clients[cur]->state!=CS_SPECTATOR)
            {
                if(following!=cur) clearfragmessages();
                if(following<0) conoutf("follow on");
                following = cur;
                followdir = dir;
                return;
            }
        }
        stopfollowing();
    }
    ICOMMAND(nextfollow, "i", (int *dir), nextfollow(*dir < 0 ? -1 : 1));


    const char *getclientmap() { return clientmap; }

    void resetgamestate()
    {
        if(m_classicsp)
        {
            clearmovables();
            clearmonsters();                 // all monsters back at their spawns for editing
            entities::resettriggers();
        }
        clearprojectiles();
        clearbouncers();
    }

    fpsent *spawnstate(fpsent *d)              // reset player state not persistent accross spawns
    {
        d->respawn();
        d->spawnstate(gamemode);
        return d;
    }

    void respawnself()
    {
        if(ispaused()) return;
        if(m_mp(gamemode))
        {
            int seq = (player1->lifesequence<<16)|((lastmillis/1000)&0xFFFF);
            if(player1->respawned!=seq) { addmsg(N_TRYSPAWN, "rc", player1); player1->respawned = seq; }
        }
        else
        {
            spawnplayer(player1);
            showscores(false);
            lasthit = 0;
            if(cmode) cmode->respawned(player1);
        }
    }

    fpsent *pointatplayer()
    {
        loopv(players) if(players[i] != player1 && intersect(players[i], player1->o, worldpos)) return players[i];
        return NULL;
    }

    void stopfollowing()
    {
        if(following<0) return;
        following = -1;
        followdir = 0;
        clearfragmessages();
        conoutf("follow off");
    }

    fpsent *followingplayer(fpsent *fallback)
    {
        if(player1->state!=CS_SPECTATOR || following<0) return fallback;
        fpsent *target = getclient(following);
        if(target && target->state!=CS_SPECTATOR) return target;
        return fallback;
    }

    fpsent *hudplayer()
    {
        if(thirdperson && allowthirdperson()) return player1;
        return followingplayer(player1);
    }

    void setupcamera()
    {
        fpsent *target = followingplayer();
        if(target)
        {
            player1->yaw = target->yaw;
            player1->pitch = target->state==CS_DEAD ? 0 : target->pitch;
            player1->o = target->o;
            player1->resetinterp();
        }
    }

    bool allowthirdperson(bool msg)
    {
        return player1->state==CS_SPECTATOR || player1->state==CS_EDITING || m_edit || !multiplayer(msg);
    }
    ICOMMAND(allowthirdperson, "b", (int *msg), intret(allowthirdperson(*msg!=0) ? 1 : 0));

    bool detachcamera()
    {
        fpsent *d = hudplayer();
        return d->state==CS_DEAD;
    }

    bool collidecamera()
    {
        switch(player1->state)
        {
            case CS_EDITING: return false;
            case CS_SPECTATOR: return followingplayer()!=NULL;
        }
        return true;
    }

    VARP(smoothmove, 0, 75, 100);
    VARP(smoothdist, 0, 32, 64);

    void predictplayer(fpsent *d, bool move)
    {
        d->o = d->newpos;
        d->yaw = d->newyaw;
        d->pitch = d->newpitch;
        d->roll = d->newroll;
        if(move)
        {
            moveplayer(d, 1, false);
            d->newpos = d->o;
        }
        float k = 1.0f - float(lastmillis - d->smoothmillis)/smoothmove;
        if(k>0)
        {
            d->o.add(vec(d->deltapos).mul(k));
            d->yaw += d->deltayaw*k;
            if(d->yaw<0) d->yaw += 360;
            else if(d->yaw>=360) d->yaw -= 360;
            d->pitch += d->deltapitch*k;
            d->roll += d->deltaroll*k;
        }
    }

    void otherplayers(int curtime)
    {
        loopv(players)
        {
            fpsent *d = players[i];
            if(d == player1 || d->ai) continue;

            if(d->state==CS_DEAD && d->ragdoll) moveragdoll(d);
            else if(!intermission)
            {
                if(lastmillis - d->lastaction >= d->gunwait) d->gunwait = 0;
                if(d->quadmillis) entities::checkquad(curtime, d);
            }

            const int lagtime = totalmillis-d->lastupdate;
            if(!lagtime || intermission) continue;
            else if(lagtime>1000 && d->state==CS_ALIVE)
            {
                d->state = CS_LAGGED;
                continue;
            }
            if(d->state==CS_ALIVE || d->state==CS_EDITING)
            {
                if(smoothmove && d->smoothmillis>0) predictplayer(d, true);
                else moveplayer(d, 1, false);
            }
            else if(d->state==CS_DEAD && !d->ragdoll && lastmillis-d->lastpain<2000) moveplayer(d, 1, true);
        }
    }

    VARFP(slowmosp, 0, 0, 1, { if(m_sp && !slowmosp) server::forcegamespeed(100); });

    void checkslowmo()
    {
        static int lastslowmohealth = 0;
        server::forcegamespeed(intermission ? 100 : clamp(player1->health, 25, 200));
        if(player1->health<player1->maxhealth && lastmillis-max(maptime, lastslowmohealth)>player1->health*player1->health/2)
        {
            lastslowmohealth = lastmillis;
            player1->health++;
        }
    }

    extern void checkextinfos();
    extern void checkseserverinfo();
    void checkgameinfo() {
        checkseserverinfo();
        checkextinfos();
    }


    void updateworld()        // main game update loop
    {
        if(!maptime) { maptime = lastmillis; maprealtime = totalmillis; return; }
        //if(!curtime) { gets2c(); if(player1->clientnum>=0) c2sinfo(); return; }
        if(!curtime || ispaused()) {
            gets2c();
            if(curtime && player1->state==CS_SPECTATOR) { fakephysicsframe(); moveplayer(player1, 10, true); }
            if(player1->clientnum>=0) c2sinfo();
            return;
        }

        playtime();
        physicsframe();
        ai::navigate();
        if(player1->state != CS_DEAD && !intermission)
        {
            if(player1->quadmillis) entities::checkquad(curtime, player1);
        }
        updateweapons(curtime);
        otherplayers(curtime);
        ai::update();
        moveragdolls();
        gets2c();
        updatemovables(curtime);
        updatemonsters(curtime);
        if(connected)
        {
            if(player1->state == CS_DEAD)
            {
                if(player1->ragdoll) moveragdoll(player1);
                else if(lastmillis-player1->lastpain<2000)
                {
                    player1->move = player1->strafe = 0;
                    moveplayer(player1, 10, true);
                }
            }
            else if(!intermission)
            {
                if(player1->ragdoll) cleanragdoll(player1);
                moveplayer(player1, 10, true);
                swayhudgun(curtime);
                entities::checkitems(player1);
                if(m_sp)
                {
                    if(slowmosp) checkslowmo();
                    if(m_classicsp) entities::checktriggers();
                }
                else if(cmode) cmode->checkitems(player1);
            }
        }
        if(player1->clientnum>=0) c2sinfo();   // do this last, to reduce the effective frame lag
    }

    VARP(savestats, 0, 1, 1);
    //basic stats
    VARP(totalplaytime, 0, 0, INT_MAX);
    VARP(totalspectime, 0, 0, INT_MAX);
    VARP(totalfrags, INT_MIN, 0, INT_MAX);
    VARP(totaldeaths, INT_MIN, 0, INT_MAX);
    VARP(totalflags, 0, 0, INT_MAX);

    // global stats - only addup and NOT on runtime
    VARP(globalplaytime, 0, 0, INT_MAX);
    VARP(globalspectime, 0, 0, INT_MAX);
    VARP(globalfrags, INT_MIN, 0, INT_MAX);
    VARP(globaldeaths, INT_MIN, 0, INT_MAX);
    VARP(globalflags, 0, 0, INT_MAX);

    // different mode var's - for local purpose
    VARP(total_e_time, 0, 0, INT_MAX);
    VARP(total_i_time, 0, 0, INT_MAX);
    VARP(totaledittime, 0, 0, INT_MAX);
    VARP(totalcooptime, 0, 0, INT_MAX); // coop OR racing
    VARP(totaldemotime, 0, 0, INT_MAX);
    VARP(totalffatime,0 , 0, INT_MAX);
    VARP(totalinstactftime, 0, 0, INT_MAX);
    VARP(totalefficctftime, 0, 0, INT_MAX);
    VARP(totalrestmodes, 0, 0, INT_MAX); // teamplay, ctf, protect, ...

    // global versions?
    VARP(global_e_time, 0, 0, INT_MAX);
    VARP(global_i_time, 0, 0, INT_MAX);
    VARP(globaledittime, 0, 0, INT_MAX);
    VARP(globalcooptime, 0, 0, INT_MAX);
    VARP(globaldemotime, 0, 0, INT_MAX);
    VARP(globalffatime, 0, 0, INT_MAX);
    VARP(globalinstactftime, 0, 0, INT_MAX);
    VARP(globalefficctftime, 0, 0, INT_MAX);
    VARP(globalrestmodes, 0, 0, INT_MAX);

void playtime()
{
    static int lastsec = 0;
    if (savestats && totalmillis - lastsec >= 1000)
    {
        int cursecs = (totalmillis - lastsec) / 1000;
        totalplaytime += cursecs; // fuck why am i not happy with only this
        lastsec += cursecs * 1000;

        // Check if the player is in spectator mode or editing mode and not in demoplayback
        // jesus fucking christ
        if (player1->state == CS_SPECTATOR && !demoplayback) { totalspectime += cursecs; return; } // only spec time
        else if (player1->state == CS_EDITING) { totaledittime += cursecs; return; } //


        if (demoplayback) { totaldemotime += cursecs; return; } // demo only
        if (m_noitems && m_efficiency && m_ctf && m_teammode && !demoplayback && !m_collect && !m_protect && !m_collect && !m_regencapture) { totalefficctftime += cursecs;  return;} // eCTF
        if (m_noitems && m_insta && m_ctf && m_teammode && !demoplayback && !m_collect && !m_protect && !m_collect && !m_regencapture) { totalinstactftime += cursecs;  return;} // iCTF
        if (m_efficiency && !demoplayback && !m_ctf  && !m_collect && !m_protect && !m_collect && !m_regencapture) { total_e_time += cursecs; return; } // effic modes in general - needed?
        if (m_insta && !demoplayback && !m_ctf  && !m_collect && !m_protect && !m_collect && !m_regencapture) { total_i_time += cursecs; return; }      // insta modes in general - needed?
        if (m_edit) { totalcooptime += cursecs; return; } //  racing OR mapping
        if (m_lobby) { totalffatime += cursecs; return; } // FFA?
        else { totalrestmodes += cursecs; } // every other mode...
        lastsec += cursecs * 1000;
    }
}

#include <cstring>
#include <cstring>

char* clearEmptyLines(const char* text, size_t* size)
{
    if (!text || !size)
        return NULL;

    size_t textLen = strlen(text);
    char* result = new char[textLen + 1];

    size_t resultIdx = 0;
    bool prevCharWasNewline = false;

    for (size_t i = 0; i < textLen; ++i)
    {
        char currentChar = text[i];

        if (currentChar == '\n')
        {
            if (!prevCharWasNewline)
            {
                // If the previous character wasn't a newline, add this newline to the result
                result[resultIdx++] = currentChar;
                prevCharWasNewline = true;
            }
        }
        else if (!isspace(currentChar))
        {
            // If the current character is not a space or a newline, add it to the result
            result[resultIdx++] = currentChar;
            prevCharWasNewline = false;
        }
    }

    result[resultIdx] = '\0';
    *size = resultIdx;

    return result;
}



//const char *reset_history_file = "reset_history.txt"; // Change this to your desired filename

/*void save_reset_history(const char *reason = nullptr) {
    const char *filename = "reset_history.txt"; // Change this to your desired filename

    size_t filesize;
    char *existingData = loadfile(filename, &filesize, true); // Load the file with UTF-8 support
    if (!existingData) {
        conoutf("Failed to open or create %s for writing.", filename);
        return;
    }

    stream *f = openutf8file(filename, "w"); // Open in write mode (overwrite the file)
    if (!f) {
        conoutf("Failed to open or create %s for writing.", filename);
        delete[] existingData; // Cleanup the loaded file data
        return;
    }

    // Append the existing content to the file
    f->write(existingData, filesize);

    time_t rawtime;
    struct tm *timeinfo;
    char buffer[80];

    time(&rawtime);
    timeinfo = localtime(&rawtime);

    strftime(buffer, sizeof(buffer), "%d.%m.%Y", timeinfo);

    f->printf("Resetdate: %s - Total stats: %02d:%02d:%02d played, %02d:%02d:%02d spectated, %d frags, %d deaths, %.2f K/D, %d flags, Session Duration: %02d:%02d:%02d",
               buffer,
               totalplaytime / 3600, (totalplaytime % 3600) / 60, totalplaytime % 60,
               totalspectime / 3600, (totalspectime % 3600) / 60, totalspectime % 60,
               totalfrags, totaldeaths, totalfrags / (totaldeaths > 0 ? totaldeaths * 1.0f : 1.00f), totalflags,
               totalmillis / 3600000, (totalmillis % 3600000) / 60000, (totalmillis % 60000) / 1000);

    if (reason) {
        f->printf(" - Reason: %s\n", reason);
    } else {
        f->printf("\n");
    }

    delete f;
    delete[] existingData; // Cleanup the loaded file data
}*/





// pls
ICOMMAND(clear_empty_lines, "s", (const char *filename), {
    if (!filename || !filename[0]) {
        conoutf("Usage: clear_empty_lines <filename>");
        return;
    }

    vector<char> lines;

    stream *file = openutf8file(filename, "r");
    if (!file) {
        conoutf("Failed to open %s for reading.", filename);
        return;
    }

    char line[4096];

    while (file->getline(line, sizeof(line))) {
        if (line[0] != '\n') {
            for (size_t i = 0; line[i] != '\0'; ++i) {
                lines.add(line[i]);
            }
        }
    }

    delete file;

    file = openutf8file(filename, "w");
    if (!file) {
        conoutf("Failed to open %s for writing.", filename);
        return;
    }

    file->write(lines.getbuf(), lines.length());
    delete file;

    conoutf("Empty lines removed from %s.", filename);
});

// reading files
ICOMMAND(display_file_contents, "s", (const char *filename), {
    if (!filename || !filename[0]) {
        conoutf("Usage: display_file_contents <filename>");
        return;
    }

    stream *file = openutf8file(filename, "r");
    if (!file) {
        conoutf("Failed to open %s for reading.", filename);
        return;
    }

    char line[4096];

    while (file->getline(line, sizeof(line))) {
        conoutf("%s", line);
    }

    delete file;
});



// Define an ICOMMAND to clean the file
ICOMMAND(cleanresetfile, "", (), {
    const char *filename = "reset_history.txt"; // Change this to your desired filename

    // Load the file with UTF-8 support
    size_t filesize;
    char *filedata = loadfile(filename, &filesize, true);
    if (!filedata) {
        conoutf("Failed to open %s for reading.", filename);
        return;
    }

    // Remove empty lines from the loaded file data
    char **lines = NULL;
    int numlines = 0;
    char *line = filedata;
    char *nextline = strchr(line, '\n');
    while (nextline) {
        *nextline = '\0'; // Null-terminate the line
        if (line[0] != '\0') {
            lines = (char **)realloc(lines, (numlines + 1) * sizeof(char *));
            lines[numlines++] = line; // Keep non-empty lines
        }
        line = nextline + 1;
        nextline = strchr(line, '\n');
    }

    // Add the last line if it's not empty
    if (line[0] != '\0') {
        lines = (char **)realloc(lines, (numlines + 1) * sizeof(char *));
        lines[numlines++] = line;
    }

    delete[] filedata; // Cleanup the loaded file data

    // Open the file in write mode to overwrite its content
    stream *f = openutf8file(filename, "w");
    if (!f) {
        conoutf("Failed to open or create %s for writing.", filename);
        return;
    }

    // Write the cleaned lines back to the file
    for (int i = 0; i < numlines; ++i) {
        f->printf("%s\n", lines[i]);
    }

    delete f;

    conoutf("File %s has been cleaned.", filename);

    free(lines); // Cleanup allocated memory
});


/*
bool resetconfirmation = false;
int resetmillis = 0;
int resetconfirmationcode = 0;

void resetlocalstats_confirm(int code)
{
    if (code == 1)
    {
        if (resetconfirmation)
        {
            int elapsedMillis = totalmillis - resetmillis;
            int resetDelayMillis = 5000; // 5000 milliseconds (adjust as needed)

            if (elapsedMillis <= resetDelayMillis)
            {
                // Reset the statistics if the confirmation is given within the specified delay
                globalplaytime += totalplaytime;
                globalfrags += totalfrags;
                globaldeaths += totaldeaths;
                globalflags += totalflags;
                globalspectime += totalspectime;
                global_e_time += total_e_time;
                global_i_time += total_i_time;
                globaledittime += totaledittime;
                globalcooptime += totalcooptime;
                globaldemotime += totaldemotime;
                globalffatime += totalffatime;
                globalinstactftime += totalinstactftime;
                globalefficctftime += totalefficctftime;
                globalrestmodes += totalrestmodes;

                // Reset the local stats
                totalplaytime = 0;
                totalfrags = 0;
                totaldeaths = 0;
                totalflags = 0;
                totalspectime = 0;
                total_e_time = 0;
                total_i_time = 0;
                totaledittime = 0;
                totalcooptime = 0;
                totaldemotime = 0;
                totalffatime = 0;
                totalinstactftime = 0;
                totalefficctftime = 0;
                totalrestmodes = 0;

                resetconfirmation = false; // reset the confirmation flag
                save_reset_history();
                conoutf("Local stats have been resetted. Global Stats have been updated.");
            }
            else
            {
                conoutf("Time limit exceeded. Reset confirmation failed.");
                resetconfirmation = false;
                return;
            }
        }
        else
        {
            conoutf("No reset confirmation pending.");
        }
    }
    else
    {
        resetconfirmationcode = code;
        conoutf("Are you sure you want to reset local stats? This can't be undone. Type 'resetstats 1' to confirm.");
        resetconfirmation = true;
        resetmillis = totalmillis; // record the current time
    }
}


*/

bool resetconfirmation = false;
int resetmillis = 0;
int resetconfirmationcode = 0;
const char *resetconfirmationreason = nullptr;
extern void save_reset_history(const char *reason);


void resetlocalstats_confirm(int code, const char *reason = nullptr)
{
    if (code == 1)
    {
        if (resetconfirmation)
        {
            int elapsedMillis = totalmillis - resetmillis;
            int resetDelayMillis = 5000; // 5000 milliseconds (adjust as needed)

            if (elapsedMillis <= resetDelayMillis)
            {
                save_reset_history(reason);

                // print out the resetted stats
                conoutf("Resetted stats: %02d:%02d:%02d gameplay, %d frags, %d deaths, %d flags",
                        totalplaytime / 3600, (totalplaytime % 3600) / 60, totalplaytime % 60,
                        totalfrags, totaldeaths, totalflags);

                // Reset the statistics if the confirmation is given within the specified delay
                globalplaytime += totalplaytime;
                globalfrags += totalfrags;
                globaldeaths += totaldeaths;
                globalflags += totalflags;
                globalspectime += totalspectime;
                global_e_time += total_e_time;
                global_i_time += total_i_time;
                globaledittime += totaledittime;
                globalcooptime += totalcooptime;
                globaldemotime += totaldemotime;
                globalffatime += totalffatime;
                globalinstactftime += totalinstactftime;
                globalefficctftime += totalefficctftime;
                globalrestmodes += totalrestmodes;

                // Reset the local stats
                totalplaytime = 0;
                totalfrags = 0;
                totaldeaths = 0;
                totalflags = 0;
                totalspectime = 0;
                total_e_time = 0;
                total_i_time = 0;
                totaledittime = 0;
                totalcooptime = 0;
                totaldemotime = 0;
                totalffatime = 0;
                totalinstactftime = 0;
                totalefficctftime = 0;
                totalrestmodes = 0;

                resetconfirmation = false; // reset the confirmation flag

                conoutf("Local stats have been resetted. Global Stats have been updated.");
            }
            else
            {
                conoutf("Time limit exceeded. Reset confirmation failed.");
                resetconfirmation = false;
                resetconfirmationreason = nullptr;
                return;
            }
        }
        else
        {
            conoutf("No reset confirmation pending.");
        }
    }
    else
    {
        resetconfirmationcode = code;
        resetconfirmationreason = reason; // store the reason
        conoutf("Are you sure you want to reset local stats? This can't be undone. Type 'resetstats 1 <reason>' to confirm.");
        resetconfirmation = true;
        resetmillis = totalmillis; // record the current time
    }
}

ICOMMAND(resetstats, "is", (int *code, char *reason), resetlocalstats_confirm(*code, reason));




    /*void resetlocalstats()
    {
        // Add the local stats to the global stats
        globalplaytime += totalplaytime;
        globalfrags += totalfrags;
        globaldeaths += totaldeaths;
        globalflags += totalflags;
        globalspectime += totalspectime;

        // Reset the local stats
        totalplaytime = 0;
        totalfrags = 0;
        totaldeaths = 0;
        totalflags = 0;
        totalspectime = 0;
    }*/


/*    void resetallvars()
{
    totalplaytime = 0;
    totalspectime = 0;
    totalfrags = 0;
    totaldeaths = 0;
    totalflags = 0;

    globalplaytime = 0;
    globalspectime = 0;
    globalfrags = 0;
    globaldeaths = 0;
    globalflags = 0;

    total_e_time = 0;
    total_i_time = 0;
    totaledittime = 0;
    totalcooptime = 0;
    totaldemotime = 0;
    totalffatime = 0;
    totalinstactftime = 0;
    totalefficctftime = 0;
    totalrestmodes = 0;

    global_e_time = 0;
    global_i_time = 0;
    globaledittime = 0;
    globalcooptime = 0;
    globaldemotime = 0;
    globalffatime = 0;
    globalinstactftime = 0;
    globalefficctftime = 0;
    globalrestmodes = 0;
    conoutf("successfully resetted all global and local stats..welcome to the new client.");
}

ICOMMAND(resetallvars, "", (), resetallvars());
*/




    // turns out, adding this method saves a lot of typing
    void formatTime(int totalSeconds, int &hours, int &minutes, int &seconds)
    {
        hours = totalSeconds / 3600;
        totalSeconds -= hours * 3600;
        minutes = totalSeconds / 60;
        totalSeconds -= minutes * 60;
        seconds = totalSeconds;
    }



    void localstats(int input)
    {
        int hoursPlayed, minutesPlayed, secondsPlayed;
        formatTime(totalplaytime, hoursPlayed, minutesPlayed, secondsPlayed);

        if (!savestats) conoutf("Local stats are currently disabled");
        else if(!input)
        {
            conoutf("Total stats: %02d:%02d:%02d played, %d frags, %d deaths, %.2f K/D, %d flags, Session Duration: %02d:%02d:%02d",
                hoursPlayed, minutesPlayed, secondsPlayed, totalfrags, totaldeaths, totalfrags / (totaldeaths > 0 ? totaldeaths * 1.f : 1.00f),
                totalflags, totalmillis / 3600000, (totalmillis % 3600000) / 60000, (totalmillis % 60000) / 1000);
        }

            // Check the input value to decide which additional stats to display
        else {
            conoutf("Total stats: %02d:%02d:%02d played, %d frags, %d deaths, %.2f K/D, %d flags",
                    hoursPlayed, minutesPlayed, secondsPlayed, totalfrags, totaldeaths, totalfrags / (totaldeaths > 0 ? totaldeaths * 1.f : 1.00f),
                    totalflags);
        }
    }

    ICOMMAND(localstats, "i", (int *input), localstats(*input));


    // global stats system incase of localstats resetted

        void globalstats()
    {
        // Calculate the global stats by adding local stats to global stats
        int globalHours, globalMinutes, globalSeconds;
        formatTime(globalplaytime + totalplaytime, globalHours, globalMinutes, globalSeconds);

        int globalFrags = globalfrags + totalfrags;
        int globalDeaths = globaldeaths + totaldeaths;
        float globalKDRatio = globalDeaths > 0 ? (float)globalFrags / globalDeaths : 0.0f;
        int globalFlags = globalflags + totalflags;

        conoutf("Total stats: %02d:%02d:%02d played, %d frags, %d deaths, %.2f K/D, %d flags",
            globalHours, globalMinutes, globalSeconds, globalFrags, globalDeaths, globalKDRatio, globalFlags);
    }

    ICOMMAND(globalstats, "", (), globalstats());


void modestats(int input)
{
    int hoursSpectated, minutesSpectated, secondsSpectated;
    formatTime(totalspectime, hoursSpectated, minutesSpectated, secondsSpectated);

    int hoursDemo, minutesDemo, secondsDemo;
    formatTime(totaldemotime, hoursDemo, minutesDemo, secondsDemo);

    int hoursEfficiency, minutesEfficiency, secondsEfficiency;
    formatTime(total_e_time, hoursEfficiency, minutesEfficiency, secondsEfficiency);

    int hoursInsta, minutesInsta, secondsInsta;
    formatTime(total_i_time, hoursInsta, minutesInsta, secondsInsta);

    int hoursEdit, minutesEdit, secondsEdit;
    formatTime(totaledittime, hoursEdit, minutesEdit, secondsEdit);

    int hoursECTF, minutesECTF, secondsECTF;
    formatTime(totalefficctftime, hoursECTF, minutesECTF, secondsECTF);

    int hoursICTF, minutesICTF, secondsICTF;
    formatTime(totalinstactftime, hoursICTF, minutesICTF, secondsICTF);

    int hoursFFA, minutesFFA, secondsFFA;
    formatTime(totalffatime, hoursFFA, minutesFFA, secondsFFA);

    int hoursCoop, minutesCoop, secondsCoop;
    formatTime(totalcooptime, hoursCoop, minutesCoop, secondsCoop);

    int hoursRest, minutesRest, secondsRest;
    formatTime(totalrestmodes, hoursRest, minutesRest, secondsRest);

    // Calculate total playtime
    int totalHours, totalMinutes, totalSeconds;
    formatTime(totalplaytime, totalHours, totalMinutes, totalSeconds);

    if (input == 1)
    {
        // Calculate percentages
        double percentSpectated = (static_cast<double>(totalspectime) / totalplaytime) * 100.0;
        double percentDemo = (static_cast<double>(totaldemotime) / totalplaytime) * 100.0;
        double percentEfficiency = (static_cast<double>(total_e_time) / totalplaytime) * 100.0;
        double percentInsta = (static_cast<double>(total_i_time) / totalplaytime) * 100.0;
        double percentEdit = (static_cast<double>(totaledittime) / totalplaytime) * 100.0;
        double percentECTF = (static_cast<double>(totalefficctftime) / totalplaytime) * 100.0;
        double percentICTF = (static_cast<double>(totalinstactftime) / totalplaytime) * 100.0;
        double percentFFA = (static_cast<double>(totalffatime) / totalplaytime) * 100.0;
        double percentCoop = (static_cast<double>(totalcooptime) / totalplaytime) * 100.0;
        double percentRest = (static_cast<double>(totalrestmodes) / totalplaytime) * 100.0;

        conoutf("Total Playtime: %02d:%02d:%02d, Spec: %.1f%%, Demo: %.1f%%, Effic: %.1f%%, Insta: %.1f%%, Edit: %.1f%%, eCTF: %.1f%%, iCTF: %.1f%%, FFA: %.1f%%, Coop: %.1f%%, Rest Modes: %.1f%%",
            totalHours, totalMinutes, totalSeconds,
            percentSpectated, percentDemo, percentEfficiency, percentInsta, percentEdit,
            percentECTF, percentICTF, percentFFA, percentCoop, percentRest);
    }
    else
    {
        conoutf("Spec: %02d:%02d:%02d, Demo: %02d:%02d:%02d, Effic: %02d:%02d:%02d, Insta: %02d:%02d:%02d, Edit: %02d:%02d:%02d, eCTF: %02d:%02d:%02d, iCTF: %02d:%02d:%02d, FFA: %02d:%02d:%02d, Coop: %02d:%02d:%02d, Rest Modes: %02d:%02d:%02d",
            hoursSpectated, minutesSpectated, secondsSpectated,
            hoursDemo, minutesDemo, secondsDemo,
            hoursEfficiency, minutesEfficiency, secondsEfficiency,
            hoursInsta, minutesInsta, secondsInsta,
            hoursEdit, minutesEdit, secondsEdit,
            hoursECTF, minutesECTF, secondsECTF,
            hoursICTF, minutesICTF, secondsICTF,
            hoursFFA, minutesFFA, secondsFFA,
            hoursCoop, minutesCoop, secondsCoop,
            hoursRest, minutesRest, secondsRest);
    }
}

ICOMMAND(modestats, "i", (int *input), modestats(*input));


    void globalmodestats(int input)
    {
        int globalHours, globalMinutes, globalSeconds;
        formatTime(globalplaytime + totalplaytime, globalHours, globalMinutes, globalSeconds);

        int globalFrags = globalfrags + totalfrags;
        int globalDeaths = globaldeaths + totaldeaths;
        float globalKDRatio = globalDeaths > 0 ? (float)globalFrags / globalDeaths : 0.0f;
        int globalFlags = globalflags + totalflags;

        int globalHoursSpectated, globalMinutesSpectated, globalSecondsSpectated;
        formatTime(globalspectime + totalspectime, globalHoursSpectated, globalMinutesSpectated, globalSecondsSpectated);

        int globalHoursDemo, globalMinutesDemo, globalSecondsDemo;
        formatTime(globaldemotime + totaldemotime, globalHoursDemo, globalMinutesDemo, globalSecondsDemo);

        int globalHoursEfficiency, globalMinutesEfficiency, globalSecondsEfficiency;
        formatTime(global_e_time + total_e_time, globalHoursEfficiency, globalMinutesEfficiency, globalSecondsEfficiency);

        int globalHoursInsta, globalMinutesInsta, globalSecondsInsta;
        formatTime(global_i_time + total_i_time, globalHoursInsta, globalMinutesInsta, globalSecondsInsta);

        int globalHoursEdit, globalMinutesEdit, globalSecondsEdit;
        formatTime(globaledittime + totaledittime, globalHoursEdit, globalMinutesEdit, globalSecondsEdit);

        int globalHoursECTF, globalMinutesECTF, globalSecondsECTF;
        formatTime(globalefficctftime + totalefficctftime, globalHoursECTF, globalMinutesECTF, globalSecondsECTF);

        int globalHoursICTF, globalMinutesICTF, globalSecondsICTF;
        formatTime(globalinstactftime + totalinstactftime, globalHoursICTF, globalMinutesICTF, globalSecondsICTF);

        int globalHoursFFA, globalMinutesFFA, globalSecondsFFA;
        formatTime(globalffatime + totalffatime, globalHoursFFA, globalMinutesFFA, globalSecondsFFA);

        int globalHoursCoop, globalMinutesCoop, globalSecondsCoop;
        formatTime(globalcooptime + totalcooptime, globalHoursCoop, globalMinutesCoop, globalSecondsCoop);

        int globalHoursRest, globalMinutesRest, globalSecondsRest;
        formatTime(globalrestmodes + totalrestmodes, globalHoursRest, globalMinutesRest, globalSecondsRest);

        if (input)
        {
            double percentSpectated = (static_cast<double>(globalspectime + totalspectime) / (globalplaytime + totalplaytime)) * 100.0;
            double percentDemo = (static_cast<double>(globaldemotime + totaldemotime) / (globalplaytime + totalplaytime)) * 100.0;
            double percentEfficiency = (static_cast<double>(global_e_time + total_e_time) / (globalplaytime + totalplaytime)) * 100.0;
            double percentInsta = (static_cast<double>(global_i_time + total_i_time) / (globalplaytime + totalplaytime)) * 100.0;
            double percentEdit = (static_cast<double>(globaledittime + totaledittime) / (globalplaytime + totalplaytime)) * 100.0;
            double percentECTF = (static_cast<double>(globalefficctftime + totalefficctftime) / (globalplaytime + totalplaytime)) * 100.0;
            double percentICTF = (static_cast<double>(globalinstactftime + totalinstactftime) / (globalplaytime + totalplaytime)) * 100.0;
            double percentFFA = (static_cast<double>(globalffatime + totalffatime) / (globalplaytime + totalplaytime)) * 100.0;
            double percentCoop = (static_cast<double>(globalcooptime + totalcooptime) / (globalplaytime + totalplaytime)) * 100.0;
            double percentRest = (static_cast<double>(globalrestmodes + totalrestmodes) / (globalplaytime + totalplaytime)) * 100.0;

            conoutf("Global Total Playtime: %02d:%02d:%02d, Spec: %.1f%%, Demo: %.1f%%, Effic: %.1f%%, Insta: %.1f%%, Edit: %.1f%%, eCTF: %.1f%%, iCTF: %.1f%%, FFA: %.1f%%, Coop: %.1f%%, Rest Modes: %.1f%%",
                globalHours, globalMinutes, globalSeconds,
                percentSpectated, percentDemo, percentEfficiency, percentInsta, percentEdit,
                percentECTF, percentICTF, percentFFA, percentCoop, percentRest);
        }
        else
        {
            conoutf("Global Total Playtime: %02d:%02d:%02d, Spec: %02d:%02d:%02d, Demo: %02d:%02d:%02d, Effic: %02d:%02d:%02d, Insta: %02d:%02d:%02d, Edit: %02d:%02d:%02d, eCTF: %02d:%02d:%02d, iCTF: %02d:%02d:%02d, FFA: %02d:%02d:%02d, Coop: %02d:%02d:%02d, Rest Modes: %02d:%02d:%02d",
                globalHours, globalMinutes, globalSeconds,
                globalHoursSpectated, globalMinutesSpectated, globalSecondsSpectated,
                globalHoursDemo, globalMinutesDemo, globalSecondsDemo,
                globalHoursEfficiency, globalMinutesEfficiency, globalSecondsEfficiency,
                globalHoursInsta, globalMinutesInsta, globalSecondsInsta,
                globalHoursEdit, globalMinutesEdit, globalSecondsEdit,
                globalHoursECTF, globalMinutesECTF, globalSecondsECTF,
                globalHoursICTF, globalMinutesICTF, globalSecondsICTF,
                globalHoursFFA, globalMinutesFFA, globalSecondsFFA,
                globalHoursCoop, globalMinutesCoop, globalSecondsCoop,
                globalHoursRest, globalMinutesRest, globalSecondsRest);
        }
    }

    ICOMMAND(globalmodestats, "i", (int *input), globalmodestats(*input));

const char *reset_history_file = "reset_history.txt"; // Change this to your desired filename

// funkt eig aber testen

/*void save_reset_history(const char *reason = nullptr)
{
    const char *filename = path(reset_history_file, true);

    size_t filesize;
    char *existingData = loadfile(filename, &filesize, true); // Load the file with UTF-8 support
    if (!existingData) {
        conoutf("Failed to open or create %s for writing.", filename);
        return;
    }

    stream *f = openutf8file(filename, "w"); // Open in write mode (overwrite the file)
    if (!f) {
        conoutf("Failed to open or create %s for writing.", filename);
        delete[] existingData; // Cleanup the loaded file data
        return;
    }

    time_t rawtime;
    struct tm *timeinfo;
    char buffer[80];

    time(&rawtime);
    timeinfo = localtime(&rawtime);

    strftime(buffer, sizeof(buffer), "%d.%m.%Y %H:%M:%S", timeinfo); // Added timestamp (hh:mm:ss)

    f->printf("Resetdate: %s - Total stats: %02d:%02d:%02d played, %02d:%02d:%02d spectated, %d frags, %d deaths, %.2f K/D, %d flags, Session Duration: %02d:%02d:%02d",
               buffer,
               totalplaytime / 3600, (totalplaytime % 3600) / 60, totalplaytime % 60,
               totalspectime / 3600, (totalspectime % 3600) / 60, totalspectime % 60,
               totalfrags, totaldeaths, totalfrags / (totaldeaths > 0 ? totaldeaths * 1.0f : 1.00f), totalflags,
               totalmillis / 3600000, (totalmillis % 3600000) / 60000, (totalmillis % 60000) / 1000);

    if (reason)
    {
        f->printf(" - Reason: %s\n", reason);
    }
    else
    {
        f->printf("\n");
    }

    delete f;
    delete[] existingData; // Cleanup the loaded file data
}
*/
/*void save_history_create_file(const char *reason = nullptr)
{
    const char *filename = path(reset_history_file, true);

    if (fileexists(filename, "r")) {
        conoutf("File %s already exists.", filename);
        return;
    }

    stream *f = openutf8file(filename, "w"); // Open in write mode (create if it doesn't exist)
    if (!f) {
        conoutf("Failed to open or create %s for writing.", filename);
        return;
    }

    time_t rawtime;
    struct tm *timeinfo;
    char buffer[80];

    time(&rawtime);
    timeinfo = localtime(&rawtime);

    strftime(buffer, sizeof(buffer), "%d.%m.%Y %H:%M:%S", timeinfo); // Added timestamp (hh:mm:ss)

    f->printf("Saved Local Stats - This system may not work as intended\n");

    f->printf("Resetdate: %s - Total stats: %02d:%02d:%02d played, %02d:%02d:%02d spectated, %d frags, %d deaths, %.2f K/D, %d flags, Session Duration: %02d:%02d:%02d",
               buffer,
               totalplaytime / 3600, (totalplaytime % 3600) / 60, totalplaytime % 60,
               totalspectime / 3600, (totalspectime % 3600) / 60, totalspectime % 60,
               totalfrags, totaldeaths, totalfrags / (totaldeaths > 0 ? totaldeaths * 1.0f : 1.00f), totalflags,
               totalmillis / 3600000, (totalmillis % 3600000) / 60000, (totalmillis % 60000) / 1000);

    if (reason)
    {
        f->printf(" - Reason: %s\n", reason);
    }
    else
    {
        f->printf("\n");
    }

    delete f;
}*/

// includes all modes?
void save_history_create_file(const char *reason = nullptr)
{
    const char *filename = path(reset_history_file, true);

    if (fileexists(filename, "r")) {
        conoutf("File %s already exists.", filename);
        return;
    }

    stream *f = openutf8file(filename, "w"); // Open in write mode (create if it doesn't exist)
    if (!f) {
        conoutf("Failed to open or create %s for writing.", filename);
        return;
    }

    time_t rawtime;
    struct tm *timeinfo;
    char buffer[80];

    time(&rawtime);
    timeinfo = localtime(&rawtime);

    strftime(buffer, sizeof(buffer), "%d.%m.%Y %H:%M:%S", timeinfo); // Added timestamp (hh:mm:ss)

    f->printf("Saved Local Stats - This system may not work as intended\n\n");

    f->printf("Resetdate: %s - Total stats: %02d:%02d:%02d played, %02d:%02d:%02d spectated, %d frags, %d deaths, %.2f K/D, %d flags, Session Duration: %02d:%02d:%02d",
               buffer,
               totalplaytime / 3600, (totalplaytime % 3600) / 60, totalplaytime % 60,
               totalspectime / 3600, (totalspectime % 3600) / 60, totalspectime % 60,
               totalfrags, totaldeaths, totalfrags / (totaldeaths > 0 ? totaldeaths * 1.0f : 1.00f), totalflags,
               totalmillis / 3600000, (totalmillis % 3600000) / 60000, (totalmillis % 60000) / 1000);

    if (reason)
    {
        f->printf(" - Reason: %s\n", reason);
    }
    else
    {
        f->printf("\n");
    }

    // Start of mode-specific stats line
    //f->printf("Mode-specific stats: ");

        int hoursSpectated, minutesSpectated, secondsSpectated;
    formatTime(totalspectime, hoursSpectated, minutesSpectated, secondsSpectated);

    int hoursDemo, minutesDemo, secondsDemo;
    formatTime(totaldemotime, hoursDemo, minutesDemo, secondsDemo);

    int hoursEfficiency, minutesEfficiency, secondsEfficiency;
    formatTime(total_e_time, hoursEfficiency, minutesEfficiency, secondsEfficiency);

    int hoursInsta, minutesInsta, secondsInsta;
    formatTime(total_i_time, hoursInsta, minutesInsta, secondsInsta);

    int hoursEdit, minutesEdit, secondsEdit;
    formatTime(totaledittime, hoursEdit, minutesEdit, secondsEdit);

    int hoursECTF, minutesECTF, secondsECTF;
    formatTime(totalefficctftime, hoursECTF, minutesECTF, secondsECTF);

    int hoursICTF, minutesICTF, secondsICTF;
    formatTime(totalinstactftime, hoursICTF, minutesICTF, secondsICTF);

    int hoursFFA, minutesFFA, secondsFFA;
    formatTime(totalffatime, hoursFFA, minutesFFA, secondsFFA);

    int hoursCoop, minutesCoop, secondsCoop;
    formatTime(totalcooptime, hoursCoop, minutesCoop, secondsCoop);

    int hoursRest, minutesRest, secondsRest;
    formatTime(totalrestmodes, hoursRest, minutesRest, secondsRest);

    // Calculate total playtime
    int totalHours, totalMinutes, totalSeconds;
    formatTime(totalplaytime, totalHours, totalMinutes, totalSeconds);

    // Calculate percentages and add them to the mode-specific stats
    double percentSpectated = (static_cast<double>(totalspectime) / (totalplaytime + globalplaytime)) * 100.0;
    double percentDemo = (static_cast<double>(totaldemotime) / (totalplaytime + globalplaytime)) * 100.0;
    double percentEfficiency = (static_cast<double>(total_e_time) / (totalplaytime + globalplaytime)) * 100.0;
    double percentInsta = (static_cast<double>(total_i_time) / (totalplaytime + globalplaytime)) * 100.0;
    double percentEdit = (static_cast<double>(totaledittime) / (totalplaytime + globalplaytime)) * 100.0;
    double percentECTF = (static_cast<double>(totalefficctftime) / (totalplaytime + globalplaytime)) * 100.0;
    double percentICTF = (static_cast<double>(totalinstactftime) / (totalplaytime + globalplaytime)) * 100.0;
    double percentFFA = (static_cast<double>(totalffatime) / (totalplaytime + globalplaytime)) * 100.0;
    double percentCoop = (static_cast<double>(totalcooptime) / (totalplaytime + globalplaytime)) * 100.0;
    double percentRest = (static_cast<double>(totalrestmodes) / (totalplaytime + globalplaytime)) * 100.0;

f->printf("\tSpec: %02d:%02d:%02d (%.1f%%), Demo: %02d:%02d:%02d (%.1f%%), Effic: %02d:%02d:%02d (%.1f%%), Insta: %02d:%02d:%02d (%.1f%%), Edit: %02d:%02d:%02d (%.1f%%),\n",
    hoursSpectated, minutesSpectated, secondsSpectated, percentSpectated,
    hoursDemo, minutesDemo, secondsDemo, percentDemo,
    hoursEfficiency, minutesEfficiency, secondsEfficiency, percentEfficiency,
    hoursInsta, minutesInsta, secondsInsta, percentInsta,
    hoursEdit, minutesEdit, secondsEdit, percentEdit);

f->printf("\teCTF: %02d:%02d:%02d (%.1f%%), iCTF: %02d:%02d:%02d (%.1f%%), FFA: %02d:%02d:%02d (%.1f%%), Coop: %02d:%02d:%02d (%.1f%%), Rest Modes: %02d:%02d:%02d (%.1f%%)\n",
    hoursECTF, minutesECTF, secondsECTF, percentECTF,
    hoursICTF, minutesICTF, secondsICTF, percentICTF,
    hoursFFA, minutesFFA, secondsFFA, percentFFA,
    hoursCoop, minutesCoop, secondsCoop, percentCoop,
    hoursRest, minutesRest, secondsRest, percentRest);



    delete f;
}


/*void save_reset_history(const char *reason = nullptr)
{
    const char *filename = path(reset_history_file, true);

    /*if (!fileexists(filename, "r")) {
        conoutf("File %s does not exist.", filename);
        save_history_create_file(reason);
        return;
    }

    size_t filesize;
    char *existingData = loadfile(filename, &filesize, true); // Load the file with UTF-8 support
    if (!existingData) {
        conoutf("File %s does not exist. Creating a new file...", filename);
        save_history_create_file(reason);
        return;
    }

    stream *f = openutf8file(filename, "w"); // Open in append mode (preserve existing data)
    if (!f) {
        conoutf("Failed to open or create %s for writing.", filename);
        delete[] existingData; // Cleanup the loaded file data
        return;
    }

    f->write(existingData, filesize);

    time_t rawtime;
    struct tm *timeinfo;
    char buffer[80];

    time(&rawtime);
    timeinfo = localtime(&rawtime);

    strftime(buffer, sizeof(buffer), "%d.%m.%Y %H:%M:%S", timeinfo); // Added timestamp (hh:mm:ss)

    f->printf("Resetdate: %s - Total stats: %02d:%02d:%02d played, %02d:%02d:%02d spectated, %d frags, %d deaths, %.2f K/D, %d flags, Session Duration: %02d:%02d:%02d",
               buffer,
               totalplaytime / 3600, (totalplaytime % 3600) / 60, totalplaytime % 60,
               totalspectime / 3600, (totalspectime % 3600) / 60, totalspectime % 60,
               totalfrags, totaldeaths, totalfrags / (totaldeaths > 0 ? totaldeaths * 1.0f : 1.00f), totalflags,
               totalmillis / 3600000, (totalmillis % 3600000) / 60000, (totalmillis % 60000) / 1000);

    if (reason)
    {
        f->printf(" - Reason: %s\n", reason);
    }
    else
    {
        f->printf("\n");
    }

    delete f;
    delete[] existingData; // Cleanup the loaded file data
}*/

// includes all modes
void save_reset_history(const char *reason = nullptr)
{
    const char *filename = path(reset_history_file, true);

    /*if (!fileexists(filename, "r")) {
        conoutf("File %s does not exist.", filename);
        save_history_create_file(reason);
        return;
    }*/

    size_t filesize;
    char *existingData = loadfile(filename, &filesize, true); // Load the file with UTF-8 support
    if (!existingData) {
        conoutf("File %s does not exist. Creating a new file...", filename);
        save_history_create_file(reason);
        return;
    }

    stream *f = openutf8file(filename, "wb"); // Open in append mode (preserve existing data)
    if (!f) {
        conoutf("Failed to open or create %s for writing.", filename);
        delete[] existingData; // Cleanup the loaded file data
        return;
    }

    f->write(existingData, filesize);

    time_t rawtime;
    struct tm *timeinfo;
    char buffer[80];

    time(&rawtime);
    timeinfo = localtime(&rawtime);

    strftime(buffer, sizeof(buffer), "%d.%m.%Y %H:%M:%S", timeinfo); // Added timestamp (hh:mm:ss)

    f->printf("Resetdate: %s - Total stats: %02d:%02d:%02d played, %02d:%02d:%02d spectated, %d frags, %d deaths, %.2f K/D, %d flags, Session Duration: %02d:%02d:%02d",
               buffer,
               totalplaytime / 3600, (totalplaytime % 3600) / 60, totalplaytime % 60,
               totalspectime / 3600, (totalspectime % 3600) / 60, totalspectime % 60,
               totalfrags, totaldeaths, totalfrags / (totaldeaths > 0 ? totaldeaths * 1.0f : 1.00f), totalflags,
               totalmillis / 3600000, (totalmillis % 3600000) / 60000, (totalmillis % 60000) / 1000);

    if (reason)
    {
        f->printf(" - Reason: %s\n", reason);
    }
    else
    {
        f->printf("\n");
    }

    // Add mode-specific stats here
    //f->printf("Mode-specific stats: "); // Start of mode-specific stats line

        int hoursSpectated, minutesSpectated, secondsSpectated;
    formatTime(totalspectime, hoursSpectated, minutesSpectated, secondsSpectated);

    int hoursDemo, minutesDemo, secondsDemo;
    formatTime(totaldemotime, hoursDemo, minutesDemo, secondsDemo);

    int hoursEfficiency, minutesEfficiency, secondsEfficiency;
    formatTime(total_e_time, hoursEfficiency, minutesEfficiency, secondsEfficiency);

    int hoursInsta, minutesInsta, secondsInsta;
    formatTime(total_i_time, hoursInsta, minutesInsta, secondsInsta);

    int hoursEdit, minutesEdit, secondsEdit;
    formatTime(totaledittime, hoursEdit, minutesEdit, secondsEdit);

    int hoursECTF, minutesECTF, secondsECTF;
    formatTime(totalefficctftime, hoursECTF, minutesECTF, secondsECTF);

    int hoursICTF, minutesICTF, secondsICTF;
    formatTime(totalinstactftime, hoursICTF, minutesICTF, secondsICTF);

    int hoursFFA, minutesFFA, secondsFFA;
    formatTime(totalffatime, hoursFFA, minutesFFA, secondsFFA);

    int hoursCoop, minutesCoop, secondsCoop;
    formatTime(totalcooptime, hoursCoop, minutesCoop, secondsCoop);

    int hoursRest, minutesRest, secondsRest;
    formatTime(totalrestmodes, hoursRest, minutesRest, secondsRest);

    // Calculate total playtime
    int totalHours, totalMinutes, totalSeconds;
    formatTime(totalplaytime, totalHours, totalMinutes, totalSeconds);

    // Calculate percentages and add them to the mode-specific stats
    double percentSpectated = (static_cast<double>(totalspectime) / (totalplaytime + globalplaytime)) * 100.0;
    double percentDemo = (static_cast<double>(totaldemotime) / (totalplaytime + globalplaytime)) * 100.0;
    double percentEfficiency = (static_cast<double>(total_e_time) / (totalplaytime + globalplaytime)) * 100.0;
    double percentInsta = (static_cast<double>(total_i_time) / (totalplaytime + globalplaytime)) * 100.0;
    double percentEdit = (static_cast<double>(totaledittime) / (totalplaytime + globalplaytime)) * 100.0;
    double percentECTF = (static_cast<double>(totalefficctftime) / (totalplaytime + globalplaytime)) * 100.0;
    double percentICTF = (static_cast<double>(totalinstactftime) / (totalplaytime + globalplaytime)) * 100.0;
    double percentFFA = (static_cast<double>(totalffatime) / (totalplaytime + globalplaytime)) * 100.0;
    double percentCoop = (static_cast<double>(totalcooptime) / (totalplaytime + globalplaytime)) * 100.0;
    double percentRest = (static_cast<double>(totalrestmodes) / (totalplaytime + globalplaytime)) * 100.0;

f->printf("\tSpec: %02d:%02d:%02d (%.1f%%), Demo: %02d:%02d:%02d (%.1f%%), Effic: %02d:%02d:%02d (%.1f%%), Insta: %02d:%02d:%02d (%.1f%%), Edit: %02d:%02d:%02d (%.1f%%),\n",
    hoursSpectated, minutesSpectated, secondsSpectated, percentSpectated,
    hoursDemo, minutesDemo, secondsDemo, percentDemo,
    hoursEfficiency, minutesEfficiency, secondsEfficiency, percentEfficiency,
    hoursInsta, minutesInsta, secondsInsta, percentInsta,
    hoursEdit, minutesEdit, secondsEdit, percentEdit);

f->printf("\teCTF: %02d:%02d:%02d (%.1f%%), iCTF: %02d:%02d:%02d (%.1f%%), FFA: %02d:%02d:%02d (%.1f%%), Coop: %02d:%02d:%02d (%.1f%%), Rest Modes: %02d:%02d:%02d (%.1f%%)\n",
    hoursECTF, minutesECTF, secondsECTF, percentECTF,
    hoursICTF, minutesICTF, secondsICTF, percentICTF,
    hoursFFA, minutesFFA, secondsFFA, percentFFA,
    hoursCoop, minutesCoop, secondsCoop, percentCoop,
    hoursRest, minutesRest, secondsRest, percentRest);



    delete f;
    delete[] existingData; // Cleanup the loaded file data
}


const char *name_history_file = "client_name_history.txt";

struct client_historyentry {
    string name;
};

extern void save_name_history_create_file();
vector<client_historyentry> client_name_history;

// Function to save client name history to a file (append mode)
void save_name_history() {
    const char *filename = path(name_history_file, true);

    size_t filesize;
    char *existingData = loadfile(filename, &filesize, true); // Load the file with UTF-8 support
    if (!existingData) {
        conoutf("File %s does not exist. Creating a new file...", filename);
        save_name_history_create_file();
        return;
    }

    stream *f = openutf8file(filename, "wb"); // Open in append mode (preserve existing data)
    if (!f) {
        conoutf("Failed to open or create %s for writing.", filename);
        delete[] existingData; // Cleanup the loaded file data
        //save_name_history_create_file();
        return;
    }

    //f->write(existingData, filesize);

    // Append the existing content to the file
    //f->printf("\n");  // Add a newline before adding new entries

    // Append the new client name history entries to the file
    loopv(client_name_history) {
        f->printf("%s\n", escapestring(client_name_history[i].name));
    }

    delete f;
    delete[] existingData; // Cleanup the loaded file data
}

// Function to create a new client name history file
void save_name_history_create_file() {
    const char *filename = path(name_history_file, true);

    if (fileexists(filename, "r")) {
        conoutf("File %s already exists.", filename);
        return;
    }

    stream *f = openutf8file(filename, "w"); // Open in write mode (create if it doesn't exist)
    if (!f) {
        conoutf("Failed to open or create %s for writing.", filename);
        return;
    }

    conoutf("Created a new file %s.", filename);
    delete f;
}

// Function to load client name history from a file
void load_name_history() {
    const char *filename = path(name_history_file, true);
    size_t filesize;
    char *filedata = loadfile(filename, &filesize, true); // Load the file with UTF-8 support
    if (!filedata) {
        conoutf("Failed to open %s for reading.", filename);
        return;
    }

    // Clear the existing client name history
    client_name_history.shrink(0);

    // Parse the loaded file data and add entries to the history
    char *line = filedata;
    char *nextline = strchr(line, '\n');
    while (nextline) {
        *nextline = '\0'; // Null-terminate the line
        client_historyentry entry;
        copystring(entry.name, line);
        client_name_history.add(entry);
        line = nextline + 1;
        nextline = strchr(line, '\n');
    }

    delete[] filedata;

    conoutf("Loaded %d entries from %s.", client_name_history.length(), filename);
}

void add_client_name_history(const char *name) {
    client_historyentry h;
    copystring(h.name, name);

    client_name_history.add(h);

    // Save the updated history to the file
    save_name_history();
}

vector<client_historyentry> find_client_name_history() {
    load_name_history();
    return client_name_history;
}

void send_client_name_history() {
    vector<client_historyentry> names = find_client_name_history();
    string history = "";

    if (names.empty()) return;

    loopv(names) {
        concatstring(history, " ");
        concatstring(history, names[i].name);
    }

    conoutf("Your name history:%s", history);
}

ICOMMAND(addclientname, "s", (const char *name), add_client_name_history(name));
ICOMMAND(showclientnamehistory, "", (), send_client_name_history());


ICOMMAND(fixfilestring, "s", (const char *filename), {
    if (!filename || !*filename) {
        conoutf("Usage: fixfilestring <filename>");
        return;
    }

    // Load the file with UTF-8 support
    size_t filesize;
    char *filedata = loadfile(filename, &filesize, true);
    if (!filedata) {
        conoutf("Failed to open %s for reading.", filename);
        return;
    }

    // Create a buffer for the modified content
    char *output = new char[filesize * 2]; // Make it large enough to handle potential expansion

    char *src = filedata;
    char *dst = output;
    bool insideQuotes = false; // Track if we're inside double quotes
    while (*src) {
        if (*src == '"') {
            insideQuotes = !insideQuotes; // Toggle insideQuotes flag on double quotes
        } else if (!insideQuotes && *src == '^') {
            src++; // Skip caret (^) symbol
            continue;
        }
        *dst++ = *src;
        src++;
    }
    *dst = '\0';

    // Open the file in write mode to overwrite its content
    stream *f = openutf8file(filename, "w");
    if (!f) {
        conoutf("Failed to open or create %s for writing.", filename);
        delete[] filedata; // Cleanup the loaded file data
        delete[] output; // Cleanup allocated memory
        return;
    }

    // Write the modified content back to the file
    f->write(output, strlen(output));
    delete f;

    conoutf("File %s has been fixed.", filename);

    delete[] filedata; // Cleanup the loaded file data
    delete[] output; // Cleanup allocated memory
});




    float proximityscore(float x, float lower, float upper)
    {
        if(x <= lower) return 1.0f;
        if(x >= upper) return 0.0f;
        float a = x - lower, b = x - upper;
        return (b * b) / (a * a + b * b);
    }

    static inline float harmonicmean(float a, float b) { return a + b > 0 ? 2 * a * b / (a + b) : 0.0f; }

    // avoid spawning near other players
    float ratespawn(dynent *d, const extentity &e)
    {
        fpsent *p = (fpsent *)d;
        vec loc = vec(e.o).addz(p->eyeheight);
        float maxrange = !m_noitems ? 400.0f : (cmode ? 300.0f : 110.0f);
        float minplayerdist = maxrange;
        loopv(players)
        {
            const fpsent *o = players[i];
            if(o == p)
            {
                if(m_noitems || (o->state != CS_ALIVE && lastmillis - o->lastpain > 3000)) continue;
            }
            else if(o->state != CS_ALIVE || isteam(o->team, p->team)) continue;

            vec dir = vec(o->o).sub(loc);
            float dist = dir.squaredlen();
            if(dist >= minplayerdist*minplayerdist) continue;
            dist = sqrtf(dist);
            dir.mul(1/dist);

            // scale actual distance if not in line of sight
            if(raycube(loc, dir, dist) < dist) dist *= 1.5f;
            minplayerdist = min(minplayerdist, dist);
        }
        float rating = 1.0f - proximityscore(minplayerdist, 80.0f, maxrange);
        return cmode ? harmonicmean(rating, cmode->ratespawn(p, e)) : rating;
    }

    void pickgamespawn(fpsent *d)
    {
        int ent = m_classicsp && d == player1 && respawnent >= 0 ? respawnent : -1;
        int tag = cmode ? cmode->getspawngroup(d) : 0;
        findplayerspawn(d, ent, tag);
    }

    void spawnplayer(fpsent *d)   // place at random spawn
    {
        pickgamespawn(d);
        spawnstate(d);
        if(d==player1)
        {
            if(editmode) d->state = CS_EDITING;
            else if(d->state != CS_SPECTATOR) d->state = CS_ALIVE;
        }
        else d->state = CS_ALIVE;
    }

    VARP(spawnwait, 0, 0, 1000);

    void respawn()
    {
        if(player1->state==CS_DEAD)
        {
            player1->attacking = false;
            int wait = cmode ? cmode->respawnwait(player1) : 0;
            if(wait>0)
            {
                lastspawnattempt = lastmillis;
                //conoutf(CON_GAMEINFO, "\f2you must wait %d second%s before respawn!", wait, wait!=1 ? "s" : "");
                return;
            }
            if(lastmillis < player1->lastpain + spawnwait) return;
            if(m_dmsp) { changemap(clientmap, gamemode); return; }    // if we die in SP we try the same map again
            respawnself();
            if(m_classicsp)
            {
                conoutf(CON_GAMEINFO, "\f2You wasted another life! The monsters stole your armour and some ammo...");
                loopi(NUMGUNS) if(i!=GUN_PISTOL && (player1->ammo[i] = savedammo[i]) > 5) player1->ammo[i] = max(player1->ammo[i]/3, 5);
            }
        }
    }
    COMMAND(respawn, "");

    // inputs

    VARP(attackspawn, 0, 1, 1);

    void doattack(bool on)
    {
        if(!connected || intermission) return;
        if((player1->attacking = on) && attackspawn) respawn();
    }

    VARP(jumpspawn, 0, 1, 1);

    bool canjump()
    {
        if(!connected || intermission) return false;
        if(jumpspawn) respawn();
        return player1->state!=CS_DEAD;
    }

    bool allowmove(physent *d)
    {
        if(d->type!=ENT_PLAYER) return true;
        return !((fpsent *)d)->lasttaunt || lastmillis-((fpsent *)d)->lasttaunt>=1000;
    }

    VARP(hitsound, 0, 0, 1);

    void damaged(int damage, fpsent *d, fpsent *actor, bool local)
    {
        if((d->state!=CS_ALIVE && d->state != CS_LAGGED && d->state != CS_SPAWNING) || intermission) return;

        recorddamage(actor, d, damage);

        if(local) damage = d->dodamage(damage);
        else if(actor==player1) return;

        fpsent *h = hudplayer();
        if(h!=player1 && actor==h && d!=actor)
        {
            if(hitsound && lasthit != lastmillis) playsound(S_HIT);
            lasthit = lastmillis;
        }
        if(d==h)
        {
            damageblend(damage);
            damagecompass(damage, actor->o);
        }
        damageeffect(damage, d, d!=h);

		ai::damaged(d, actor);

        if(m_sp && slowmosp && d==player1 && d->health < 1) d->health = 1;

        if(d->health<=0) { if(local) killed(d, actor); }
        else if(d==h) playsound(S_PAIN6);
        else playsound(S_PAIN1+rnd(5), &d->o);
    }

    VARP(deathscore, 0, 1, 1);

    void deathstate(fpsent *d, bool restore)
    {
        d->state = CS_DEAD;
        d->lastpain = lastmillis;
        if(!restore)
        {
            gibeffect(max(-d->health, 0), d->vel, d);
            d->deaths++;
        }
        if(d==player1)
        {
            if(deathscore) showscores(true);
            disablezoom();
            if(!restore) loopi(NUMGUNS) savedammo[i] = player1->ammo[i];
            d->attacking = false;
            //d->pitch = 0;
            d->roll = 0;
            playsound(S_DIE1+rnd(2));
        }
        else
        {
            d->move = d->strafe = 0;
            d->resetinterp();
            d->smoothmillis = 0;
            playsound(S_DIE1+rnd(2), &d->o);
        }
    }

    VARP(teamcolorfrags, 0, 1, 1);

    void killed(fpsent *d, fpsent *actor)
    {
        if(d->state==CS_EDITING)
        {
            d->editstate = CS_DEAD;
            d->deaths++;
            if(d!=player1) d->resetinterp();
            return;
        }
        else if((d->state!=CS_ALIVE && d->state != CS_LAGGED && d->state != CS_SPAWNING) || intermission) return;

        if(cmode) cmode->died(d, actor);

        fpsent *h = followingplayer(player1);
        int contype = d==h || actor==h ? CON_FRAG_SELF : CON_FRAG_OTHER;
        const char *dname = "", *aname = "";
        if(m_teammode && teamcolorfrags)
        {
            dname = teamcolorname(d, "you");
            aname = teamcolorname(actor, "you");
        }
        else
        {
            dname = colorname(d, NULL, "", "", "you");
            aname = colorname(actor, NULL, "", "", "you");
        }
        if(actor->type==ENT_AI)
            conoutf(contype, "\f2%s got killed by %s!", dname, aname);
        else if(d==actor || actor->type==ENT_INANIMATE)
            conoutf(contype, "\f2%s suicided%s", dname, d==player1 ? "!" : "");
        else if(isteam(d->team, actor->team))
        {
            contype |= CON_TEAMKILL;
            if(actor==player1) conoutf(contype, "\f6%s fragged a teammate (%s)", aname, dname);
            else if(d==player1) conoutf(contype, "\f6%s got fragged by a teammate (%s)", dname, aname);
            else conoutf(contype, "\f2%s fragged a teammate (%s)", aname, dname);
        }
        else
        {
            if(d==player1) conoutf(contype, "\f2%s got fragged by %s", dname, aname);
            else conoutf(contype, "\f2%s fragged %s", aname, dname);
        }
        if(d==h || actor==h) {
            if(d==actor) addfragmessage(NULL, dname, HICON_TOKEN-HICON_FIST);
            else addfragmessage(aname, dname, d->lasthitpushgun);
        }

        deathstate(d);
		ai::killed(d, actor);
    }

    void timeupdate(int secs)
    {
        server::timeupdate(secs);
        if(secs > 0)
        {
            maplimit = lastmillis + secs*1000;
        }
        else
        {
            intermission = true;
            player1->attacking = false;
            if(cmode) cmode->gameover();
            conoutf(CON_GAMEINFO, "\f2intermission:");
            conoutf(CON_GAMEINFO, "\f2game has ended!");
            if(m_ctf) conoutf(CON_GAMEINFO, "\f2player frags: %d, flags: %d, deaths: %d", player1->frags, player1->flags, player1->deaths);
            else if(m_collect) conoutf(CON_GAMEINFO, "\f2player frags: %d, skulls: %d, deaths: %d", player1->frags, player1->flags, player1->deaths);
            else conoutf(CON_GAMEINFO, "\f2player frags: %d, deaths: %d", player1->frags, player1->deaths);
            int accuracy = (player1->totaldamage*100)/max(player1->totalshots, 1);
            conoutf(CON_GAMEINFO, "\f2player total damage dealt: %d, damage wasted: %d, accuracy(%%): %d", player1->totaldamage, player1->totalshots-player1->totaldamage, accuracy);
            if(m_sp) spsummary(accuracy);

            showscores(true);
            disablezoom();

            execident("intermission");
        }
    }

    ICOMMAND(getfrags, "", (), intret(hudplayer()->frags));
    ICOMMAND(getflags, "", (), intret(hudplayer()->flags));
    ICOMMAND(getdeaths, "", (), intret(hudplayer()->deaths));
    ICOMMAND(gettotaldamage, "", (), intret(playerdamage(NULL, DMG_DEALT)));
    ICOMMAND(gettotalshots, "", (), intret(playerdamage(NULL, DMG_POTENTIAL)));

    vector<fpsent *> clients;

    fpsent *newclient(int cn)   // ensure valid entity
    {
        if(cn < 0 || cn > max(0xFF, MAXCLIENTS + MAXBOTS))
        {
            neterr("clientnum", false);
            return NULL;
        }

        if(cn == player1->clientnum) return player1;

        while(cn >= clients.length()) clients.add(NULL);
        if(!clients[cn])
        {
            fpsent *d = new fpsent;
            d->clientnum = cn;
            clients[cn] = d;
            players.add(d);
        }
        return clients[cn];
    }

    fpsent *getclient(int cn)   // ensure valid entity
    {
        if(cn == player1->clientnum) return player1;
        return clients.inrange(cn) ? clients[cn] : NULL;
    }

    void clientdisconnected(int cn, bool notify)
    {
        if(!clients.inrange(cn)) return;
        if(following==cn)
        {
            if(followdir) nextfollow(followdir);
            else stopfollowing();
        }
        unignore(cn);
        fpsent *d = clients[cn];
        if(!d) return;
        if(notify && d->name[0]) conoutf("\f4leave:\f7 %s", colorname(d));
        removeweapons(d);
        removetrackedparticles(d);
        removetrackeddynlights(d);
        if(cmode) cmode->removeplayer(d);
        players.removeobj(d);
        DELETEP(clients[cn]);
        cleardynentcache();
    }

    void clearclients(bool notify)
    {
        loopv(clients) if(clients[i]) clientdisconnected(i, notify);
    }

    void initclient()
    {
        player1 = spawnstate(new fpsent);
        filtertext(player1->name, "unnamed", false, false, MAXNAMELEN);
        players.add(player1);
    }

    VARP(showmodeinfo, 0, 1, 1);

    void startgame()
    {
        clearmovables();
        clearmonsters();

        clearprojectiles();
        clearbouncers();
        clearragdolls();

        clearteaminfo();

        // reset perma-state
        loopv(players)
        {
            fpsent *d = players[i];
            d->frags = d->flags = 0;
            d->deaths = 0;
            d->totaldamage = 0;
            d->totalshots = 0;
            d->maxhealth = 100;
            d->lifesequence = -1;
            d->respawned = d->suicided = -2;
            d->stats.reset();
            if(d->extdatawasinit <= 0) d->extdatawasinit = -1;

        }

        setclientmode();

        intermission = false;
        maptime = maprealtime = 0;
        maplimit = -1;

        if(cmode)
        {
            cmode->preload();
            cmode->setup();
        }

        conoutf(CON_GAMEINFO, "\f2game mode is %s", server::modename(gamemode));

        if(m_sp)
        {
            defformatstring(scorename, "bestscore_%s", getclientmap());
            const char *best = getalias(scorename);
            if(*best) conoutf(CON_GAMEINFO, "\f2try to beat your best score so far: %s", best);
        }
        else
        {
            const char *info = m_valid(gamemode) ? gamemodes[gamemode - STARTGAMEMODE].info : NULL;
            if(showmodeinfo && info) conoutf(CON_GAMEINFO, "\f0%s", info);
        }

        if(player1->playermodel != playermodel) switchplayermodel(playermodel);

        showscores(false);
        disablezoom();
        lasthit = 0;

        execident("mapstart");
    }

    void loadingmap(const char *name)
    {
        execident("playsong");
    }

    void startmap(const char *name)   // called just after a map load
    {
        ai::savewaypoints();
        ai::clearwaypoints(true);

        respawnent = -1; // so we don't respawn at an old spot
        if(!m_mp(gamemode)) spawnplayer(player1);
        else findplayerspawn(player1, -1);
        entities::resetspawns();
        copystring(clientmap, name ? name : "");

        sendmapinfo();
    }

    const char *getmapinfo()
    {
        return showmodeinfo && m_valid(gamemode) ? gamemodes[gamemode - STARTGAMEMODE].info : NULL;
    }

    const char *getscreenshotinfo()
    {
        return server::modename(gamemode, NULL);
    }

    void physicstrigger(physent *d, bool local, int floorlevel, int waterlevel, int material)
    {
        if(d->type==ENT_INANIMATE) return;
        if     (waterlevel>0) { if(material!=MAT_LAVA) playsound(S_SPLASH1, d==player1 ? NULL : &d->o); }
        else if(waterlevel<0) playsound(material==MAT_LAVA ? S_BURN : S_SPLASH2, d==player1 ? NULL : &d->o);
        if     (floorlevel>0) { if(d==player1 || d->type!=ENT_PLAYER || ((fpsent *)d)->ai) msgsound(S_JUMP, d); }
        else if(floorlevel<0) { if(d==player1 || d->type!=ENT_PLAYER || ((fpsent *)d)->ai) msgsound(S_LAND, d); }
    }

    void dynentcollide(physent *d, physent *o, const vec &dir)
    {
        switch(d->type)
        {
            case ENT_AI: if(dir.z > 0) stackmonster((monster *)d, o); break;
            case ENT_INANIMATE: if(dir.z > 0) stackmovable((movable *)d, o); break;
        }
    }

    void msgsound(int n, physent *d)
    {
        if(!d || d==player1)
        {
            addmsg(N_SOUND, "ci", d, n);
            playsound(n);
        }
        else
        {
            if(d->type==ENT_PLAYER && ((fpsent *)d)->ai)
                addmsg(N_SOUND, "ci", d, n);
            playsound(n, &d->o);
        }
    }

    int numdynents() { return players.length()+monsters.length()+movables.length(); }

    dynent *iterdynents(int i)
    {
        if(i<players.length()) return players[i];
        i -= players.length();
        if(i<monsters.length()) return (dynent *)monsters[i];
        i -= monsters.length();
        if(i<movables.length()) return (dynent *)movables[i];
        return NULL;
    }

    bool duplicatename(fpsent *d, const char *name = NULL, const char *alt = NULL)
    {
        if(!name) name = d->name;
        if(alt && d != player1 && !strcmp(name, alt)) return true;
        loopv(players) if(d!=players[i] && !strcmp(name, players[i]->name)) return true;
        return false;
    }

    static string cname[3];
    static int cidx = 0;

    const char *colorname(fpsent *d, const char *name, const char *prefix, const char *suffix, const char *alt)
    {
        if(!name) name = alt && d == player1 ? alt : d->name;
        extern int showclientnum;
        bool dup = showclientnum == 2 || !name[0] || duplicatename(d, name, alt) || d->aitype != AI_NONE;
        if(dup || prefix[0] || suffix[0])
        {
            cidx = (cidx+1)%3;
            if(dup) formatstring(cname[cidx], d->aitype == AI_NONE ? "%s%s \fs\f5(%d)\fr%s" : "%s%s \fs\f5[%d]\fr%s", prefix, name, d->clientnum, suffix);
            //if(dup) formatstring(cname[cidx], "%s%s \fs\f4[%d]\fr%s", prefix, name, d->clientnum, suffix);
            else formatstring(cname[cidx], "%s%s%s", prefix, name, suffix);
            return cname[cidx];
        }
        return name;
    }

    VARP(teamcolortext, 0, 1, 1);

    const char *teamcolorname(fpsent *d, const char *alt)
    {
        if(!teamcolortext || !m_teammode || d->state==CS_SPECTATOR) return colorname(d, NULL, "", "", alt);
        return colorname(d, NULL, isteam(d->team, player1->team) ? "\fs\f1" : "\fs\f3", "\fr", alt);
    }

    const char *teamcolor(const char *name, bool sameteam, const char *alt)
    {
        if(!teamcolortext || !m_teammode) return sameteam || !alt ? name : alt;
        cidx = (cidx+1)%3;
        formatstring(cname[cidx], sameteam ? "\fs\f1%s\fr" : "\fs\f3%s\fr", sameteam || !alt ? name : alt);
        return cname[cidx];
    }

    const char *teamcolor(const char *name, const char *team, const char *alt)
    {
        return teamcolor(name, team && isteam(team, player1->team), alt);
    }

    VARP(teamsounds, 0, 1, 1);

    void teamsound(bool sameteam, int n, const vec *loc)
    {
        playsound(n, loc, NULL, teamsounds ? (m_teammode && sameteam ? SND_USE_ALT : SND_NO_ALT) : 0);
    }

    void teamsound(fpsent *d, int n, const vec *loc)
    {
        teamsound(isteam(d->team, player1->team), n, loc);
    }

    void suicide(physent *d)
    {
        if(d==player1 || (d->type==ENT_PLAYER && ((fpsent *)d)->ai))
        {
            if(d->state!=CS_ALIVE) return;
            fpsent *pl = (fpsent *)d;
            if(!m_mp(gamemode)) killed(pl, pl);
            else
            {
                int seq = (pl->lifesequence<<16)|((lastmillis/1000)&0xFFFF);
                if(pl->suicided!=seq) { addmsg(N_SUICIDE, "rc", pl); pl->suicided = seq; }
            }
        }
        else if(d->type==ENT_AI) suicidemonster((monster *)d);
        else if(d->type==ENT_INANIMATE) suicidemovable((movable *)d);
    }
    ICOMMAND(suicide, "", (), suicide(player1));

    bool needminimap() { return m_ctf || m_protect || m_hold || m_capture || m_collect; }

    void drawicon(int icon, float x, float y, float sz)
    {
        settexture("packages/hud/items.png");
        float tsz = 0.25f, tx = tsz*(icon%4), ty = tsz*(icon/4);
        gle::defvertex(2);
        gle::deftexcoord0();
        gle::begin(GL_TRIANGLE_STRIP);
        gle::attribf(x,    y);    gle::attribf(tx,     ty);
        gle::attribf(x+sz, y);    gle::attribf(tx+tsz, ty);
        gle::attribf(x,    y+sz); gle::attribf(tx,     ty+tsz);
        gle::attribf(x+sz, y+sz); gle::attribf(tx+tsz, ty+tsz);
        gle::end();
    }

    float abovegameplayhud(int w, int h)
    {
        switch(hudplayer()->state)
        {
            case CS_EDITING:
            case CS_SPECTATOR:
                return 1;
            default:
                return 1650.0f/1800.0f;
        }
    }

    int ammohudup[3] = { GUN_CG, GUN_RL, GUN_GL },
        ammohuddown[3] = { GUN_RIFLE, GUN_SG, GUN_PISTOL },
        ammohudcycle[7] = { -1, -1, -1, -1, -1, -1, -1 };

    ICOMMAND(ammohudup, "V", (tagval *args, int numargs),
    {
        loopi(3) ammohudup[i] = i < numargs ? getweapon(args[i].getstr()) : -1;
    });

    ICOMMAND(ammohuddown, "V", (tagval *args, int numargs),
    {
        loopi(3) ammohuddown[i] = i < numargs ? getweapon(args[i].getstr()) : -1;
    });

    ICOMMAND(ammohudcycle, "V", (tagval *args, int numargs),
    {
        loopi(7) ammohudcycle[i] = i < numargs ? getweapon(args[i].getstr()) : -1;
    });

    VARP(ammohud, 0, 1, 1);

    void drawammohud(fpsent *d)
    {
        float x = HICON_X + 2*HICON_STEP, y = HICON_Y, sz = HICON_SIZE;
        pushhudmatrix();
        hudmatrix.scale(1/3.2f, 1/3.2f, 1);
        flushhudmatrix();
        float xup = (x+sz)*3.2f, yup = y*3.2f + 0.1f*sz;
        loopi(3)
        {
            int gun = ammohudup[i];
            if(gun < GUN_FIST || gun > GUN_PISTOL || gun == d->gunselect || !d->ammo[gun]) continue;
            drawicon(HICON_FIST+gun, xup, yup, sz);
            yup += sz;
        }
        float xdown = x*3.2f - sz, ydown = (y+sz)*3.2f - 0.1f*sz;
        loopi(3)
        {
            int gun = ammohuddown[3-i-1];
            if(gun < GUN_FIST || gun > GUN_PISTOL || gun == d->gunselect || !d->ammo[gun]) continue;
            ydown -= sz;
            drawicon(HICON_FIST+gun, xdown, ydown, sz);
        }
        int offset = 0, num = 0;
        loopi(7)
        {
            int gun = ammohudcycle[i];
            if(gun < GUN_FIST || gun > GUN_PISTOL) continue;
            if(gun == d->gunselect) offset = i + 1;
            else if(d->ammo[gun]) num++;
        }
        float xcycle = (x+sz/2)*3.2f + 0.5f*num*sz, ycycle = y*3.2f-sz;
        loopi(7)
        {
            int gun = ammohudcycle[(i + offset)%7];
            if(gun < GUN_FIST || gun > GUN_PISTOL || gun == d->gunselect || !d->ammo[gun]) continue;
            xcycle -= sz;
            drawicon(HICON_FIST+gun, xcycle, ycycle, sz);
        }
        pophudmatrix();
    }

    VARP(healthcolors, 0, 1, 1);

    void drawhudicons(fpsent *d)
    {
        pushhudmatrix();
        hudmatrix.scale(2, 2, 1);
        flushhudmatrix();

        defformatstring(health, "%d", d->state==CS_DEAD ? 0 : d->health);
        bvec healthcolor = bvec::hexcolor(healthcolors && !m_insta ? (d->state==CS_DEAD ? 0x808080 : (d->health<=25 ? 0xFF0000 : (d->health<=50 ? 0xFF8000 : (d->health<=100 ? 0xFFFFFF : 0x40C0FF)))) : 0xFFFFFF);
        draw_text(health, (HICON_X + HICON_SIZE + HICON_SPACE)/2, HICON_TEXTY/2, healthcolor.r, healthcolor.g, healthcolor.b);
        if(d->state!=CS_DEAD)
        {
            if(d->armour) draw_textf("%d", (HICON_X + HICON_STEP + HICON_SIZE + HICON_SPACE)/2, HICON_TEXTY/2, d->armour);
            draw_textf("%d", (HICON_X + 2*HICON_STEP + HICON_SIZE + HICON_SPACE)/2, HICON_TEXTY/2, d->ammo[d->gunselect]);
        }

        pophudmatrix();

        if(d->state != CS_DEAD && d->maxhealth > 100)
        {
            float scale = 0.66f;
            pushhudmatrix();
            hudmatrix.scale(scale, scale, 1);
            flushhudmatrix();

            float width, height;
            text_boundsf(health, width, height);
            draw_textf("/%d", (HICON_X + HICON_SIZE + HICON_SPACE + width*2)/scale, (HICON_TEXTY + height)/scale, d->maxhealth);

            pophudmatrix();
        }

        drawicon(HICON_HEALTH, HICON_X, HICON_Y);
        if(d->state!=CS_DEAD)
        {
            if(d->armour) drawicon(HICON_BLUE_ARMOUR+d->armourtype, HICON_X + HICON_STEP, HICON_Y);
            drawicon(HICON_FIST+d->gunselect, HICON_X + 2*HICON_STEP, HICON_Y);
            if(d->quadmillis) drawicon(HICON_QUAD, HICON_X + 3*HICON_STEP, HICON_Y);
            if(ammohud) drawammohud(d);
        }
    }

    VARP(gameclock, 0, 0, 1);
    FVARP(gameclockscale, 1e-3f, 0.75f, 1e3f);
    HVARP(gameclockcolour, 0, 0xFFFFFF, 0xFFFFFF);
    VARP(gameclockalpha, 0, 255, 255);
    HVARP(gameclocklowcolour, 0, 0xFFC040, 0xFFFFFF);
    VARP(gameclockalign, -1, 0, 1);
    FVARP(gameclockx, 0, 0.50f, 1);
    FVARP(gameclocky, 0, 0.03f, 1);

    void drawgameclock(int w, int h)
    {
        int secs = max(maplimit-lastmillis + 999, 0)/1000, mins = secs/60;
        secs %= 60;

        defformatstring(buf, "%d:%02d", mins, secs);
        int tw = 0, th = 0;
        text_bounds(buf, tw, th);

        vec2 offset = vec2(gameclockx, gameclocky).mul(vec2(w, h).div(gameclockscale));
        if(gameclockalign == 1) offset.x -= tw;
        else if(gameclockalign == 0) offset.x -= tw/2.0f;
        offset.y -= th/2.0f;

        pushhudmatrix();
        hudmatrix.scale(gameclockscale, gameclockscale, 1);
        flushhudmatrix();

        int color = mins < 1 ? gameclocklowcolour : gameclockcolour;
        draw_text(buf, int(offset.x), int(offset.y), (color>>16)&0xFF, (color>>8)&0xFF, color&0xFF, gameclockalpha);

        pophudmatrix();
    }

    extern int hudscore;
    extern void drawhudscore(int w, int h);

    VARP(ammobar, 0, 0, 1);
    VARP(ammobaralign, -1, 0, 1);
    VARP(ammobarhorizontal, 0, 0, 1);
    VARP(ammobarflip, 0, 0, 1);
    VARP(ammobarhideempty, 0, 1, 1);
    VARP(ammobarsep, 0, 20, 500);
    VARP(ammobarcountsep, 0, 20, 500);
    FVARP(ammobarcountscale, 0.5, 1.5, 2);
    FVARP(ammobarx, 0, 0.025f, 1.0f);
    FVARP(ammobary, 0, 0.5f, 1.0f);
    FVARP(ammobarscale, 0.1f, 0.5f, 1.0f);

    void drawammobarcounter(const vec2 &center, const fpsent *p, int gun)
    {
        vec2 icondrawpos = vec2(center).sub(HICON_SIZE / 2);
        int alpha = p->ammo[gun] ? 0xFF : 0x7F;
        gle::color(bvec(0xFF, 0xFF, 0xFF), alpha);
        drawicon(HICON_FIST + gun, icondrawpos.x, icondrawpos.y);

        int fw, fh; text_bounds("000", fw, fh);
        float labeloffset = HICON_SIZE / 2.0f + ammobarcountsep + ammobarcountscale * (ammobarhorizontal ? fh : fw) / 2.0f;
        vec2 offsetdir = (ammobarhorizontal ? vec2(0, 1) : vec2(1, 0)).mul(ammobarflip ? -1 : 1);
        vec2 labelorigin = vec2(offsetdir).mul(labeloffset).add(center);

        pushhudmatrix();
        hudmatrix.translate(labelorigin.x, labelorigin.y, 0);
        hudmatrix.scale(ammobarcountscale, ammobarcountscale, 1);
        flushhudmatrix();

        defformatstring(label, "%d", p->ammo[gun]);
        int tw, th; text_bounds(label, tw, th);
        vec2 textdrawpos = vec2(-tw, -th).div(2);
        float ammoratio = (float)p->ammo[gun] / itemstats[gun-GUN_SG].add;
        bvec color = bvec::hexcolor(p->ammo[gun] == 0 || ammoratio >= 1.0f ? 0xFFFFFF : (ammoratio >= 0.5f ? 0xFFC040 : 0xFF0000));
        draw_text(label, textdrawpos.x, textdrawpos.y, color.r, color.g, color.b, alpha);

        pophudmatrix();
    }

    static inline bool ammobargunvisible(const fpsent *d, int gun)
    {
        return d->ammo[gun] > 0 || d->gunselect == gun;
    }

    void drawammobar(int w, int h, fpsent *p)
    {
        if(m_insta) return;

        int NUMPLAYERGUNS = GUN_PISTOL - GUN_SG + 1;
        int numvisibleguns = NUMPLAYERGUNS;
        if(ammobarhideempty) loopi(NUMPLAYERGUNS) if(!ammobargunvisible(p, GUN_SG + i)) numvisibleguns--;

        vec2 origin = vec2(ammobarx, ammobary).mul(vec2(w, h).div(ammobarscale));
        vec2 offsetdir = ammobarhorizontal ? vec2(1, 0) : vec2(0, 1);
        float stepsize = HICON_SIZE + ammobarsep;
        float initialoffset = (ammobaralign - 1) * (numvisibleguns - 1) * stepsize / 2;

        pushhudmatrix();
        hudmatrix.scale(ammobarscale, ammobarscale, 1);
        flushhudmatrix();

        int numskippedguns = 0;
        loopi(NUMPLAYERGUNS) if(ammobargunvisible(p, GUN_SG + i) || !ammobarhideempty)
        {
            float offset = initialoffset + (i - numskippedguns) * stepsize;
            vec2 drawpos = vec2(offsetdir).mul(offset).add(origin);
            drawammobarcounter(drawpos, p, GUN_SG + i);
        }
        else numskippedguns++;

        pophudmatrix();
    }

    vector<fragmessage> fragmessages; // oldest first, newest at the end

    VARP(fragmsg, 0, 0, 2);
    VARP(fragmsgmax, 1, 3, 10);
    VARP(fragmsgmillis, 0, 3000, 10000);
    VARP(fragmsgfade, 0, 1, 1);
    FVARP(fragmsgx, 0, 0.5f, 1.0f);
    FVARP(fragmsgy, 0, 0.15f, 1.0f);
    FVARP(fragmsgscale, 0, 0.5f, 1.0f);

    void addfragmessage(const char *aname, const char *vname, int gun)
    {
        fragmessages.growbuf(fragmsgmax);
        fragmessages.shrink(min(fragmessages.length(), fragmsgmax));
        if(fragmessages.length()>=fragmsgmax) fragmessages.remove(0, fragmessages.length()-fragmsgmax+1);
        fragmessages.add(fragmessage(aname, vname, gun));
    }

    void clearfragmessages()
    {
        fragmessages.shrink(0);
    }

    void drawfragmessages(int w, int h)
    {
        if(fragmessages.empty()) return;

        float stepsize = (3*HICON_SIZE)/2;
        vec2 origin = vec2(fragmsgx, fragmsgy).mul(vec2(w, h).div(fragmsgscale));

        pushhudmatrix();
        hudmatrix.scale(fragmsgscale, fragmsgscale, 1);
        flushhudmatrix();

        for(int i = fragmessages.length()-1; i>=0; i--)
        {
            fragmessage &m = fragmessages[i];

            if(lastmillis-m.fragtime > fragmsgmillis + (fragmsgfade ? 255 : 0))
            {
                // all messages before i are older, so remove all of them
                fragmessages.remove(0, i+1);
                break;
            }

            int alpha = 255 - max(0, lastmillis-m.fragtime-fragmsgmillis);

            vec2 drawposcenter = vec2(0, (fragmessages.length()-1-i)*stepsize).add(origin);

            int tw, th; vec2 drawpos;
            if(m.attackername[0])
            {
                text_bounds(m.attackername, tw, th);
                drawpos = vec2(-2*(tw+HICON_SIZE), -th).div(2).add(drawposcenter);
                draw_text(m.attackername, drawpos.x, drawpos.y, 0xFF, 0xFF, 0xFF, alpha);
            }

            drawpos = vec2(drawposcenter).sub(HICON_SIZE / 2);
            gle::color(bvec(0xFF, 0xFF, 0xFF), alpha);
            drawicon(HICON_FIST + m.weapon, drawpos.x, drawpos.y);

            text_bounds(m.victimname, tw, th);
            drawpos = vec2(2*HICON_SIZE, -th).div(2).add(drawposcenter);
            draw_text(m.victimname, drawpos.x, drawpos.y, 0xFF, 0xFF, 0xFF, alpha);
        }

        pophudmatrix();
    }


    void gameplayhud(int w, int h)
    {
        pushhudmatrix();
        hudmatrix.scale(h/1800.0f, h/1800.0f, 1);
        flushhudmatrix();

        if(player1->state==CS_SPECTATOR)
        {
            int pw, ph, tw, th, fw, fh;
            text_bounds("  ", pw, ph);
            text_bounds("SPECTATOR", tw, th);
            th = max(th, ph);
            fpsent *f = followingplayer();
            text_bounds(f ? colorname(f) : " ", fw, fh);
            fh = max(fh, ph);
            draw_text("SPECTATOR", w*1800/h - tw - pw, 1600 - th - fh);
            if(f)
            {
                int color = statuscolor(f, 0xFFFFFF);
                draw_text(colorname(f), w*1800/h - fw - pw, 1600 - fh, (color>>16)&0xFF, (color>>8)&0xFF, color&0xFF);
            }
        }

        fpsent *d = hudplayer();
        if(d->state!=CS_EDITING)
        {
            if(d->state!=CS_SPECTATOR) drawhudicons(d);
            if(cmode) cmode->drawhud(d, w, h);
        }

        pophudmatrix();

        if(d->state!=CS_EDITING && d->state!=CS_SPECTATOR && d->state!=CS_DEAD)
        {
            if(ammobar) drawammobar(w, h, d);
        }

        if(!m_edit && !m_sp)
        {
            if(gameclock) drawgameclock(w, h);
            if(hudscore) drawhudscore(w, h);
            if(fragmsg==1 || (fragmsg==2 && !m_insta)) drawfragmessages(w, h);
        }
    }

    int clipconsole(int w, int h)
    {
        if(cmode) return cmode->clipconsole(w, h);
        return 0;
    }

    VARP(teamcrosshair, 0, 1, 1);
    VARP(hitcrosshair, 0, 425, 1000);

    const char *defaultcrosshair(int index)
    {
        switch(index)
        {
            case 2: return "data/hit.png";
            case 1: return "data/teammate.png";
            default: return "data/crosshair.png";
        }
    }

    int selectcrosshair(vec &color)
    {
        fpsent *d = hudplayer();
        if(d->state==CS_SPECTATOR || d->state==CS_DEAD) return -1;

        if(d->state!=CS_ALIVE) return 0;

        int crosshair = 0;
        if(lasthit && lastmillis - lasthit < hitcrosshair) crosshair = 2;
        else if(teamcrosshair)
        {
            dynent *o = intersectclosest(d->o, worldpos, d);
            if(o && o->type==ENT_PLAYER && isteam(((fpsent *)o)->team, d->team))
            {
                crosshair = 1;
                color = vec(0, 0, 1);
            }
        }

        if(crosshair!=1 && !editmode && !m_insta)
        {
            if(d->health<=25) color = vec(1, 0, 0);
            else if(d->health<=50) color = vec(1, 0.5f, 0);
        }
        if(d->gunwait) color.mul(0.5f);
        return crosshair;
    }

    void lighteffects(dynent *e, vec &color, vec &dir)
    {
#if 0
        fpsent *d = (fpsent *)e;
        if(d->state!=CS_DEAD && d->quadmillis)
        {
            float t = 0.5f + 0.5f*sinf(2*M_PI*lastmillis/1000.0f);
            color.y = color.y*(1-t) + t;
        }
#endif
    }

    int maxsoundradius(int n)
    {
        switch(n)
        {
            case S_JUMP:
            case S_LAND:
            case S_WEAPLOAD:
            case S_ITEMAMMO:
            case S_ITEMHEALTH:
            case S_ITEMARMOUR:
            case S_ITEMPUP:
            case S_ITEMSPAWN:
            case S_NOAMMO:
            case S_PUPOUT:
                return 340;
            default:
                return 500;
        }
    }

    bool serverinfostartcolumn(g3d_gui *g, int i)
    {
        static const char * const names[] = { "ping ", "players ", "mode ", "map ", "time ", "master ", "host ", "port ", "description " };
        static const float struts[] =       { 7,       7,          12.5f,   14,      7,      8,         14,      7,       24.5f };
        if(size_t(i) >= sizeof(names)/sizeof(names[0])) return false;
        g->pushlist();
        g->text(names[i], 0xFFFFFF, !i ? " " : NULL);
        if(struts[i]) g->strut(struts[i]);
        g->mergehits(true);
        return true;
    }

    void serverinfoendcolumn(g3d_gui *g, int i)
    {
        g->mergehits(false);
        g->column(i);
        g->poplist();
    }

    const char *mastermodecolor(int n, const char *unknown)
    {
        return (n>=MM_START && size_t(n-MM_START)<sizeof(mastermodecolors)/sizeof(mastermodecolors[0])) ? mastermodecolors[n-MM_START] : unknown;
    }

    const char *mastermodeicon(int n, const char *unknown)
    {
        return (n>=MM_START && size_t(n-MM_START)<sizeof(mastermodeicons)/sizeof(mastermodeicons[0])) ? mastermodeicons[n-MM_START] : unknown;
    }

    // put this somewhere else you lazy bum
    int cubecasecmp(const char *s1, const char *s2, int n)
    {
        if(!s1 || !s2) return !s2 - !s1;
        while(n-- > 0)
        {
            int c1 = cubelower(*s1++), c2 = cubelower(*s2++);
            if(c1 != c2) return c1 - c2;
            if(!c1) break;
        }
        return 0;
    }

    char *cubecasefind(const char *haystack, const char *needle)
    {
        if(haystack && needle) for(const char *h = haystack, *n = needle;;)
            {
            int hc = cubelower(*h++), nc = cubelower(*n++);
            if(!nc) return (char*)h - (n - needle);
            if(hc != nc)
            {
                if(!hc) break;
                n = needle;
                h = ++haystack;
            }
        }
        return NULL;
    }

    SVAR(filterservers, "");


    bool serverinfoentry(g3d_gui *g, int i, const char *name, int port, const char *sdesc, const char *map, int ping, const vector<int> &attr, int np)
    {

        if (*filterservers) {
            if (!cubecasefind(sdesc, filterservers) &&
            !cubecasefind(map, filterservers) &&
            (attr.length() < 2 || !cubecasefind(server::modename(attr[1], ""), filterservers))) {
            return false;
            }
        }


        if(ping < 0 || attr.empty() || attr[0]!=PROTOCOL_VERSION)
        {
            switch(i)
            {
                case 0:
                    if(g->button(" ", 0xFFFFDD, "serverunk")&G3D_UP) return true;
                    break;

                case 1:
                case 2:
                case 3:
                case 4:
                case 5:
                    if(g->button(" ", 0xFFFFDD)&G3D_UP) return true;
                    break;

                case 6:
                    if(g->buttonf("%s ", 0xFFFFDD, NULL, name)&G3D_UP) return true;
                    break;

                case 7:
                    if(g->buttonf("%d ", 0xFFFFDD, NULL, port)&G3D_UP) return true;
                    break;

                case 8:
                    if(ping < 0)
                    {
                        if(g->button(sdesc, 0xFFFFDD)&G3D_UP) return true;
                    }
                    else if(g->buttonf("[%s protocol] ", 0xFFFFDD, NULL, attr.empty() ? "unknown" : (attr[0] < PROTOCOL_VERSION ? "older" : "newer"))&G3D_UP) return true;
                    break;
            }
            return false;
        }

        switch(i)
        {
            case 0:
            {
                const char *icon = attr.inrange(3) && np >= attr[3] ? "serverfull" : (attr.inrange(4) ? mastermodeicon(attr[4], "serverunk") : "serverunk");
                if(g->buttonf("%d ", 0xFFFFDD, icon, ping)&G3D_UP) return true;
                break;
            }

            case 1:
                if(attr.length()>=4)
                {
                    if(g->buttonf(np >= attr[3] ? "\f3%d/%d " : "%d/%d ", 0xFFFFDD, NULL, np, attr[3])&G3D_UP) return true;
                }
                else if(g->buttonf("%d ", 0xFFFFDD, NULL, np)&G3D_UP) return true;
                break;

            case 2:
                if(g->buttonf("%s ", 0xFFFFDD, NULL, attr.length()>=2 ? server::modename(attr[1], "") : "")&G3D_UP) return true;
                break;

            case 3:
                if(g->buttonf("%.25s ", 0xFFFFDD, NULL, map)&G3D_UP) return true;
                break;

            case 4:
                if(attr.length()>=3 && attr[2] > 0)
                {
                    int secs = clamp(attr[2], 0, 59*60+59),
                        mins = secs/60;
                    secs %= 60;
                    if(g->buttonf("%d:%02d ", 0xFFFFDD, NULL, mins, secs)&G3D_UP) return true;
                }
                else if(g->buttonf(" ", 0xFFFFDD)&G3D_UP) return true;
                break;
            case 5:
                if(g->buttonf("%s%s ", 0xFFFFDD, NULL, attr.length()>=5 ? mastermodecolor(attr[4], "") : "", attr.length()>=5 ? server::mastermodename(attr[4], "") : "")&G3D_UP) return true;
                break;

            case 6:
                if(g->buttonf("%s ", 0xFFFFDD, NULL, name)&G3D_UP) return true;
                break;

            case 7:
                if(g->buttonf("%d ", 0xFFFFDD, NULL, port)&G3D_UP) return true;
                break;

            case 8:
                if(g->buttonf("%.25s", 0xFFFFDD, NULL, sdesc)&G3D_UP) return true;
                break;
        }
        return false;
    }

    // any data written into this vector will get saved with the map data. Must take care to do own versioning, and endianess if applicable. Will not get called when loading maps from other games, so provide defaults.
    void writegamedata(vector<char> &extras) {}
    void readgamedata(vector<char> &extras) {}

    const char *savedconfig() { return "config.cfg"; }
    const char *restoreconfig() { return "restore.cfg"; }
    const char *defaultconfig() { return "data/defaults.cfg"; }
    const char *autoexec() { return "autoexec.cfg"; }
    const char *savedservers() { return "servers.cfg"; }

    void loadconfigs()
    {
        execident("playsong");

        execfile("auth.cfg", false);
    }
}

