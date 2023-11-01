// creation of scoreboard
#include "game.h"

namespace game
{
    VARP(scoreboard2d, 0, 1, 1);
    VARP(showservinfo, 0, 1, 1);
    VARP(showclientnum, 0, 0, 2);
    VARP(showpj, 0, 0, 1);
    VARP(showping, 0, 1, 2);
    VARP(showspectators, 0, 1, 1);
    VARP(showspectatorping, 0, 0, 1);
    VARP(highlightscore, 0, 1, 2);
    VARP(showconnecting, 0, 0, 1);
    VARP(hidefrags, 0, 1, 1);
    VARP(showdeaths, 0, 0, 1);
    VARP(showkpd, 0, 0, 1);
    VARP(showaccuracy, 0, 0, 1);
    VARP(showdamage, 0, 0, 2);
    VARP(showflags, 0, 1, 1);
    VARP(showspecicons, 0, 1, 1);
    VARP(showctfflagicons, 0, 1, 1);
    VARP(showteamsize, 0, 1, 1); // maybe for all vs all's?
    VARP(showplayericon, 0, 1, 1);
    VARP(alternativescoreboard, 0, 0, 1);
    HVARP(textcolor, 0, 0xA0A0A0, 0xFFFFFF);
    VARP(hideheader, 0, 0, 1);
    VARP(spacevarp, 0, 0, 1000);
    VARP(showpjcolor, 0, 1, 1);
    VARP(noseperator, 0, 0, 1);

    void resetscoreboardcolours()
        {
            int oldvalue = textcolor;
            textcolor = 0xA0A0A0;
            conoutf("Text color reset from 0x%X to 0x%X", oldvalue, textcolor);

        }

    ICOMMAND(resetscoreboardcolours, "", (), resetscoreboardcolours());



    static hashset<teaminfo> teaminfos;

    void clearteaminfo()
    {
        teaminfos.clear();
    }

    void setteaminfo(const char *team, int frags)
    {
        teaminfo *t = teaminfos.access(team);
        if(!t) { t = &teaminfos[team]; copystring(t->team, team, sizeof(t->team)); }
        t->frags = frags;
    }

    static inline bool playersort(const fpsent *a, const fpsent *b)
    {
        if(a->state==CS_SPECTATOR)
        {
            if(b->state==CS_SPECTATOR) return strcmp(a->name, b->name) < 0;
            else return false;
        }
        else if(b->state==CS_SPECTATOR) return true;
        if(m_ctf || m_collect)
        {
            if(a->flags > b->flags) return true;
            if(a->flags < b->flags) return false;
        }
        if(a->frags > b->frags) return true;
        if(a->frags < b->frags) return false;
        return strcmp(a->name, b->name) < 0;
    }

    void getbestplayers(vector<fpsent *> &best)
    {
        loopv(players)
        {
            fpsent *o = players[i];
            if(o->state!=CS_SPECTATOR) best.add(o);
        }
        best.sort(playersort);
        while(best.length() > 1 && best.last()->frags < best[0]->frags) best.drop();
    }

    void getbestteams(vector<const char *> &best)
    {
        if(cmode && cmode->hidefrags())
        {
            vector<teamscore> teamscores;
            cmode->getteamscores(teamscores);
            teamscores.sort(teamscore::compare);
            while(teamscores.length() > 1 && teamscores.last().score < teamscores[0].score) teamscores.drop();
            loopv(teamscores) best.add(teamscores[i].team);
        }
        else
        {
            int bestfrags = INT_MIN;
            enumerate(teaminfos, teaminfo, t, bestfrags = max(bestfrags, t.frags));
            if(bestfrags <= 0) loopv(players)
            {
                fpsent *o = players[i];
                if(o->state!=CS_SPECTATOR && !teaminfos.access(o->team) && best.htfind(o->team) < 0) { bestfrags = 0; best.add(o->team); }
            }
            enumerate(teaminfos, teaminfo, t, if(t.frags >= bestfrags) best.add(t.team));
        }
    }

    struct scoregroup : teamscore
    {
        vector<fpsent *> players;
    };
    static vector<scoregroup *> groups;
    static vector<fpsent *> spectators;

    static inline bool scoregroupcmp(const scoregroup *x, const scoregroup *y)
    {
        if(!x->team)
        {
            if(y->team) return false;
        }
        else if(!y->team) return true;
        if(x->score > y->score) return true;
        if(x->score < y->score) return false;
        if(x->players.length() > y->players.length()) return true;
        if(x->players.length() < y->players.length()) return false;
        return x->team && y->team && strcmp(x->team, y->team) < 0;
    }

    #define COL_WHITE 0xFFFFDD //0xFFFF80
    #define COL_GRAY 0xA0A0A0
    #define COL_BACKGROUND 0x9A9A9A
    #define COL_RED 0xED2B2C
    #define COL_BLUE 0x2E82FF
    #define COL_YELLOW 0xFFC040
    #define COL_GREEN 0x40FF80
    #define COL_ORANGE 0xFF8000
    #define COL_MAGENTA 0xC040C0
    #define COL_MASTER COL_GREEN
    #define COL_AUTH COL_MAGENTA
    #define COL_ADMIN COL_ORANGE
    //#define p1xtester (o==hudplayer() && highlightscore && (multiplayer(false) || demoplayback || players.length() > 1) ? COL_YELLOW : 0xFFFF80)

    static int groupplayers()
    {
        int numgroups = 0;
        spectators.setsize(0);
        loopv(players)
        {
            fpsent *o = players[i];
            //int p1xtester (o==hudplayer() && highlightscore == 2 && (multiplayer(false) || demoplayback || players.length() > 1) ? 0xFFC040 : 0xFFFF80);

            if(!showconnecting && !o->name[0]) continue;
            if(o->state==CS_SPECTATOR) { spectators.add(o); continue; }
            const char *team = m_teammode && o->team[0] ? o->team : NULL;
            bool found = false;
            loopj(numgroups)
            {
                scoregroup &g = *groups[j];
                if(team!=g.team && (!team || !g.team || strcmp(team, g.team))) continue;
                g.players.add(o);
                found = true;
            }
            if(found) continue;
            if(numgroups>=groups.length()) groups.add(new scoregroup);
            scoregroup &g = *groups[numgroups++];
            g.team = team;
            if(!team) g.score = 0;
            else if(cmode && cmode->hidefrags()) g.score = cmode->getteamscore(o->team);
            else { teaminfo *ti = teaminfos.access(team); g.score = ti ? ti->frags : 0; }
            g.players.setsize(0);
            g.players.add(o);
        }
        loopi(numgroups) groups[i]->players.sort(playersort);
        spectators.sort(playersort);
        groups.sort(scoregroupcmp, 0, numgroups);
        return numgroups;
    }

    int statuscolor(fpsent *d, int color)
    {
        if(highlightscore == 1){
            if(d->privilege)
            {
                color = d->privilege>=PRIV_ADMIN ? 0xFF8000 : (d->privilege>=PRIV_AUTH ? 0xC040C0 : 0x40FF80);
                if(d->state==CS_DEAD) color = (color>>1)&0x7F7F7F;
            }
            else if(d->state==CS_DEAD) color = 0x606060;
            return color;
        }
        else{
            if(d->privilege) color = d->privilege>=PRIV_ADMIN ? COL_ADMIN : (d->privilege>=PRIV_AUTH ? COL_AUTH : COL_MASTER);
            if(d->state==CS_DEAD) color = (color>>1)&0x7F7F7F;
            return color;
        }
    }



    void renderscoreboard(g3d_gui &g, bool firstpass)
    {
        const ENetAddress *address = connectedpeer();
        if(showservinfo && address)
        {
            string hostname;
            if(enet_address_get_host_ip(address, hostname, sizeof(hostname)) >= 0)
            {
                if(servinfo[0]) g.titlef("%.25s", alternativescoreboard ? 0xFFFFFF : 0xFFFF80, NULL, servinfo);
                else g.titlef("%s:%d", alternativescoreboard ? textcolor : 0xFFFF80, NULL, hostname, address->port);
            }
        }



        g.pushlist();
        g.spring();
        g.text(server::modename(gamemode), alternativescoreboard ? 0xFFFFFF : 0xFFFF80);
        noseperator ? g.space(1.5f) : g.separator();
        const char *mname = getclientmap();
        g.text(mname[0] ? mname : "[new map]", alternativescoreboard ? 0xFFFFFF : 0xFFFF80);
        extern int gamespeed;
        if(gamespeed != 100) { noseperator ? g.space(1.5f) : g.separator(); g.textf("%d.%02dx", alternativescoreboard ? 0xFFFFFF : 0xFFFF80, NULL, gamespeed/100, gamespeed%100); }
        if(m_timed && mname[0] && (maplimit >= 0 || intermission))
        {
            noseperator ? g.space(1.5f) : g.separator();
            if(intermission) g.text("intermission", alternativescoreboard ? 0xFFFFFF : 0xFFFF80);
            else
            {
                int secs = max(maplimit-lastmillis+999, 0)/1000, mins = secs/60;
                secs %= 60;
                g.pushlist();
                g.strut(mins >= 10 ? 4.5f : 3.5f);
                g.textf("%d:%02d", alternativescoreboard ? 0xFFFFFF : 0xFFFF80, NULL, mins, secs);
                g.poplist();
            }
        }
        if(ispaused()) { noseperator ? g.space(1.5f) : g.separator(); g.text("paused", alternativescoreboard ? textcolor : 0xFFFF80); }
        g.spring();
        g.poplist();

        noseperator ? g.space(1) : g.separator();

        int numgroups = groupplayers();
        loopk(numgroups)
        {

            if((k%2)==0) g.pushlist(); // horizontal

            scoregroup &sg = *groups[k];
            //int bgcolor = sg.team && m_teammode ? (isteam(player1->team, sg.team) ? 0x3030C0 : 0xC03030) : 0,
            int bgcolor = sg.team && m_teammode ? (isteam(player1->team, sg.team) ? 0x3030C0 : 0xC03030) : 0;
            int fgcolor = alternativescoreboard ? textcolor : 0xFFFF80;
            int teamfgcolor = alternativescoreboard ? (sg.team && m_teammode ? (isteam(player1->team, sg.team) ? 0x6496FF  : 0xFF4B19) : 0) : fgcolor;
            //#define p1xtester (o==hudplayer() && highlightscore && (multiplayer(false) || demoplayback || players.length() > 1) ? COL_YELLOW : COL_WHITE)
            #define p1xtester (o == player1 && highlightscore == 2 && (multiplayer(false) || demoplayback || players.length() > 1) ? COL_YELLOW : COL_WHITE)

            loopv(players)
            {
            fpsent *o = players[i];
            //int p1xtester (o==hudplayer() && highlightscore == 2 && (multiplayer(false) || demoplayback || players.length() > 1) ? 0xFFC040 : 0xFFFF80);
            }


            g.pushlist(); // vertical
            g.pushlist(); // horizontal

            #define loopscoregroup(o, b) \
                loopv(sg.players) \
                { \
                    fpsent *o = sg.players[i]; \
                    b; \
                }

            g.pushlist();
            if(sg.team && m_teammode)
            {
                g.pushlist();
                if(!alternativescoreboard) g.background(bgcolor, numgroups>1 ? 3 : 5);
                g.strut(1);
                g.poplist();
            }
            g.text("", 0, " ");
            loopscoregroup(o,
            {
                if(o==player1 && highlightscore && (multiplayer(false) || demoplayback || players.length() > 1))
                {
                    g.pushlist();
                    if(highlightscore == 1) g.background(0x808080, numgroups>1 ? 3 : 5); // 0xFF8000
                }

                const playermodelinfo &mdl = getplayermodelinfo(o);
                const char *icon = !showplayericon ? NULL : (sg.team && m_teammode ? (isteam(player1->team, sg.team) ? mdl.blueicon : mdl.redicon) : mdl.ffaicon);
                g.text("", 0, icon);
                if(o==player1 && highlightscore && (multiplayer(false) || demoplayback || players.length() > 1)) g.poplist();
            });

            g.poplist();

            if(sg.team && m_teammode)
            {
                g.pushlist(); // vertical


                g.pushlist();
                if(sg.score>=10000) g.textf("%s: WIN", teamfgcolor, NULL, sg.team);
                else g.textf("%s: %d", teamfgcolor, NULL, sg.team, sg.score);
                if(showteamsize) {
					g.spring();
					g.textf("#%d ", teamfgcolor, NULL, sg.players.length());
				}
				g.poplist();

                g.pushlist(); // horizontal
            }

            if(!cmode || !cmode->hidefrags() || !hidefrags)
            {
                g.pushlist();
                g.strut(6);
                g.text("frags", fgcolor);
                //loopscoregroup(o, g.textf("%d", 0xFFFFDD, NULL, o->frags));
                loopscoregroup(o, g.textf((showflags && o->flags) ? "%d/%d" : "%d", highlightscore == 2 ? p1xtester : 0xFFFFDD, NULL, o->frags, o->flags));
                g.poplist();
            }

            if(showdeaths)
            {
                g.pushlist();
                g.strut(6);
                g.text("deaths", fgcolor);
                loopscoregroup(o, g.textf("%d", highlightscore == 2 ? p1xtester : 0xFFFFDD, NULL, o->deaths));
                g.poplist();
            }

            if(m_ctf && showctfflagicons) {
                g.pushlist();
                g.text("", 0x000000);
                loopscoregroup(o, {
                    // check if the player is carrying a flag
                    int flagteam = hasflag(o);
                    if(flagteam > 0) { // has flag
                        // choose the correct icon
                        const char *icon =
                            m_hold ? "../hud/blip_neutral_flag.png"
                            : isteam(player1->team, flagteam == 1 ? "good" : "evil") ? "../hud/blip_blue_flag.png" : "../hud/blip_red_flag.png";

                        g.text("", 0x000000, icon);
                    } else {
                        g.text("", 0x000000);
                    }
                });
                g.poplist();
            }
            g.pushlist();
            g.text("name", fgcolor);
            g.strut(12);
            loopscoregroup(o,
            {
                g.textf("%s ", statuscolor(o, highlightscore == 2 ? p1xtester : 0xFFFFDD), NULL, colorname(o));
            });
            g.poplist();

            if (showkpd)
            {
                g.pushlist();
                g.strut(5);
                g.text("kpd", fgcolor);
                loopscoregroup(o, g.textf("%.1f", highlightscore == 2 ? p1xtester : 0xFFFFDD, NULL, (float)o->frags / max(1, o->deaths)));
                g.poplist();
            }

            if (showaccuracy)
            {
                g.pushlist();
                g.strut(5);
                g.text("acc", fgcolor);
                loopscoregroup(o, g.textf("%.0f%%", highlightscore == 2 ? p1xtester : 0xFFFFDD, NULL, playeraccuracy(o)));
                g.poplist();
            }

            if (showdamage)
            {
                g.pushlist();
                g.strut(6);
                g.text("dmg", fgcolor);
                loopscoregroup(o, {
                    float dmg = (float) showdamage == 1 ? playerdamage(o, DMG_DEALT) : playernetdamage(o);
                    const char *fmt = "%.0f";
                    if (fabs(dmg) > 1000.0f) { fmt = "%.1fk"; dmg = dmg / 1000.0f; }
                    g.textf(fmt, highlightscore == 2 ? p1xtester : 0xFFFFDD, NULL, dmg);
                });
                g.poplist();
            }


            if(multiplayer(false) || demoplayback)
            {
                if(showpj || showping) g.space(1);

                if(showpj && showping <= 1)
                {
                    g.pushlist();
                    g.strut(6);
                    g.text("pj", fgcolor);
                    loopscoregroup(o,
                    {
                        if(o->state==CS_LAGGED) g.text("LAG", 0xFFFFDD);
                        else g.textf("%d", highlightscore == 2 ? p1xtester : 0xFFFFDD, NULL, o->plag);
                    });
                    g.poplist();
                }

                if(showping == 2)
                {
                    g.pushlist();
                    g.strut(6);

                    g.pushlist();
                    g.text(showpj ? "ping/pj" : "ping", fgcolor);
                    g.poplist();

                    loopscoregroup(o,
                    {
                        fpsent *p = o->ownernum >= 0 ? getclient(o->ownernum) : o;
                        if(!p) p = o;
                        g.pushlist();
                        //const char *pjcolor = (showpjcolor && p->plag > 0) ? (p->plag < 40 ? "\f0" : "\f3") : "";
                        const char *pjcolor = (showpjcolor && p->plag > 0) ? (p->plag > 50 ? "\f3" : (p->plag > 40 ? "\f6" : "\f0")) : "";
                        if(p->state==CS_LAGGED) g.text("\f3LAG", highlightscore == 2 ? p1xtester : 0xFFFFDD);
                        else g.textf(showpj ? "%d%s %d" : "%d" , highlightscore == 2 ? p1xtester : 0xFFFFDD, NULL, p->ping, pjcolor, p->plag);
                        g.poplist();

                    });
                    g.poplist();
                }

                /*{
                    g.pushlist();
                    g.text("ping", fgcolor);
                    g.strut(6);
                    loopscoregroup(o,
                    {
                        fpsent *p = o->ownernum >= 0 ? getclient(o->ownernum) : o;
                        if(!p) p = o;
                        /*const char *pjcolor;
                        if (p->plag == 0) {
                            pjcolor = "";
                        } else if (p->plag < 40) {
                            pjcolor = "\f0";
                        } else {
                            pjcolor = "\f3";
                        }
                        const char *pjcolor = (showpjcolor && p->plag > 0) ? (p->plag < 40 ? "\f0" : "\f3") : "";
                        if(p->state==CS_LAGGED) g.text("\f3LAG", 0xFFFFDD);
                        else g.textf("%d%s %d", 0xFFFFDD, NULL, p->ping, pjcolor, p->plag);
                        //else g.textf(showpj ? "%d%s %d" : "%d" , 0xFFFFDD, NULL, p->ping, pjcolor, p->plag);
                    });
                    g.poplist();
                }*/

                else if(showping)
                {
                    g.pushlist();
                    g.text("ping", fgcolor);
                    g.strut(6);
                    loopscoregroup(o,
                    {
                        fpsent *p = o->ownernum >= 0 ? getclient(o->ownernum) : o;
                        if(!p) p = o;
                        if(!showpj && p->state==CS_LAGGED) g.text("LAG", highlightscore == 2 ? p1xtester : 0xFFFFDD);
                        else g.textf("%d", highlightscore == 2 ? p1xtester : 0xFFFFDD, NULL, p->ping);
                    });
                    g.poplist();
                }
            }

            if(showclientnum == 1 || player1->privilege>=PRIV_MASTER)
            {
                g.space(1);
                g.pushlist();
                g.text("cn", fgcolor);
                loopscoregroup(o, g.textf("%d", highlightscore == 2 ? p1xtester : 0xFFFFDD, NULL, o->clientnum));
                g.poplist();
            }

            if(sg.team && m_teammode)
            {
                g.poplist(); // horizontal
                g.poplist(); // vertical
            }

            g.poplist(); // horizontal
            g.poplist(); // vertical

            if(k+1<numgroups && (k+1)%2) g.space(2);
            else g.poplist(); // horizontal
        }

        if(showspectators && spectators.length())
        {
                #define loopspectators(o, b) \
                    loopv(spectators) \
                    { \
                        fpsent *o = spectators[i]; \
                        b; \
                    }
            if(showclientnum || player1->privilege>=PRIV_MASTER)
            {
                g.pushlist();

                g.pushlist();
                if(showteamsize) {
					g.textf("spectator #%d", alternativescoreboard ? textcolor : 0xFFFF80, " ", spectators.length());
					g.strut(15);
				}
				else {
                	g.text("spectator ", alternativescoreboard ? textcolor : 0xFFFF80, " ");
					g.strut(12);
				}
                loopspectators(o,
                {
                    if(o == player1 && highlightscore)
                    {
                        g.pushlist();
                        if(highlightscore == 1) g.background(0x808080, 3);
                    }
                    const playermodelinfo &mdl = getplayermodelinfo(o);
                    g.text(colorname(o), statuscolor(o, p1xtester), !showplayericon ? "blank.png" : (showplayericon && showspecicons ? mdl.ffaicon : "spectator"));
                    if(o == player1 && highlightscore) g.poplist();
                });


                g.poplist();

                if((multiplayer(false) || demoplayback) && showspectatorping)
                {
                    g.space(1);
                    g.pushlist();
                    g.text("ping", alternativescoreboard ? textcolor : 0xFFFF80);
                    g.strut(6);
                loopspectators(o,
                {
                    fpsent *o = spectators[i]; \
                    fpsent *p = o->ownernum >= 0 ? getclient(o->ownernum) : o; \
                    if (!p) p = o;
                    if (p->state == CS_LAGGED) g.text("LAG", p1xtester);
                    else g.textf("%d", p1xtester, NULL, p->ping);
                });
                    g.poplist();
                }
                if(showclientnum == 1) {
                    g.space(1);
                    g.pushlist();
                    g.text("cn", alternativescoreboard ? textcolor : 0xFFFF80);
                    loopspectators(o, g.textf("%d", p1xtester, NULL, o->clientnum));
                    g.poplist();
                }
                    g.poplist();

            }
        else if (showplayericon && showspecicons) {
                if(showteamsize) {
					g.textf("spectator #%d", alternativescoreboard ? textcolor : 0xFFFF80, " ", spectators.length());
					g.strut(15);
				}
				else {
                	g.text("spectator ", alternativescoreboard ? textcolor : 0xFFFF80, " ");
					g.strut(12);
				}

                loopspectators(o,
                {
                if (o == player1 && highlightscore)
                {
                    g.pushlist();
                    if (highlightscore == 1) g.background(0x808080, 3);
                }
                const playermodelinfo &mdl = getplayermodelinfo(o);
                if (o == player1 && highlightscore) g.poplist();
                    g.text(colorname(o), statuscolor(o, p1xtester), !showplayericon ? "blank.png" : (showplayericon && showspecicons ? mdl.ffaicon : "spectator"));
                });



        }

            else
            {
                g.textf("%d spectator%s", alternativescoreboard ? textcolor : 0xFFFF80, " ", spectators.length(), spectators.length()!=1 ? "s" : "");
                loopspectators(o,
                {
                    if((i%3)==0)
                    {
                        g.pushlist();
                        //g.text("", 0xFFFFDD, "spectator");
                        g.text("", p1xtester, !showplayericon ? "blank.png" : "spectator");
                    }
                    //fpsent *o = spectators[i];
                    if(o==player1 && highlightscore)
                    {
                        g.pushlist();
                        if(highlightscore == 1) g.background(0x808080);
                    }
                    g.text(colorname(o), statuscolor(o, p1xtester));
                    if(o==player1 && highlightscore) g.poplist();
                    if(i+1<spectators.length() && (i+1)%3) g.space(1);
                    else g.poplist();
                    });
                }
            }
    }

    struct scoreboardgui : g3d_callback
    {
        bool showing;
        vec menupos;
        int menustart;

        scoreboardgui() : showing(false) {}

        void show(bool on)
        {
            if(!showing && on)
            {
                menupos = menuinfrontofplayer();
                menustart = starttime();
            }
            showing = on;
        }

        void gui(g3d_gui &g, bool firstpass)
        {
            g.start(menustart, 0.03f, NULL, false);
            renderscoreboard(g, firstpass);
            g.end();
        }

        void render()
        {
            if(showing) g3d_addgui(this, menupos, (scoreboard2d ? GUI_FORCE_2D : GUI_2D | GUI_FOLLOW) | GUI_BOTTOM);
        }

    } scoreboard;

    void g3d_gamemenus()
    {
        scoreboard.render();
    }

    VARFN(scoreboard, showscoreboard, 0, 0, 1, scoreboard.show(showscoreboard!=0));

    void showscores(bool on)
    {
        showscoreboard = on ? 1 : 0;
        scoreboard.show(on);
    }
    ICOMMAND(showscores, "D", (int *down), showscores(*down!=0));

    VARP(hudscore, 0, 0, 1);
    FVARP(hudscorescale, 1e-3f, 1.0f, 1e3f);
    VARP(hudscorealign, -1, 0, 1);
    FVARP(hudscorex, 0, 0.50f, 1);
    FVARP(hudscorey, 0, 0.03f, 1);
    HVARP(hudscoreplayercolour, 0, 0x60A0FF, 0xFFFFFF);
    HVARP(hudscoreenemycolour, 0, 0xFF4040, 0xFFFFFF);
    VARP(hudscorealpha, 0, 255, 255);
    VARP(hudscoresep, 0, 200, 1000);

    void drawhudscore(int w, int h)
    {
        int numgroups = groupplayers();
        if(!numgroups) return;

        scoregroup *g = groups[0];
        int score = INT_MIN, score2 = INT_MIN;
        bool best = false;
        if(m_teammode)
        {
            score = g->score;
            best = isteam(player1->team, g->team);
            if(numgroups > 1)
            {
                if(best) score2 = groups[1]->score;
                else for(int i = 1; i < groups.length(); ++i) if(isteam(player1->team, groups[i]->team)) { score2 = groups[i]->score; break; }
                if(score2 == INT_MIN)
                {
                    fpsent *p = followingplayer(player1);
                    if(p->state==CS_SPECTATOR) score2 = groups[1]->score;
                }
            }
        }
        else
        {
            fpsent *p = followingplayer(player1);
            score = g->players[0]->frags;
            best = p == g->players[0];
            if(g->players.length() > 1)
            {
                if(best || p->state==CS_SPECTATOR) score2 = g->players[1]->frags;
                else score2 = p->frags;
            }
        }
        if(score == score2 && !best) best = true;

        score = clamp(score, -999, 9999);
        defformatstring(buf, "%d", score);
        int tw = 0, th = 0;
        text_bounds(buf, tw, th);

        string buf2;
        int tw2 = 0, th2 = 0;
        if(score2 > INT_MIN)
        {
            score2 = clamp(score2, -999, 9999);
            formatstring(buf2, "%d", score2);
            text_bounds(buf2, tw2, th2);
        }

        int fw = 0, fh = 0;
        text_bounds("00", fw, fh);
        fw = max(fw, max(tw, tw2));

        vec2 offset = vec2(hudscorex, hudscorey).mul(vec2(w, h).div(hudscorescale));
        if(hudscorealign == 1) offset.x -= 2*fw + hudscoresep;
        else if(hudscorealign == 0) offset.x -= (2*fw + hudscoresep) / 2.0f;
        vec2 offset2 = offset;
        offset.x += (fw-tw)/2.0f;
        offset.y -= th/2.0f;
        offset2.x += fw + hudscoresep + (fw-tw2)/2.0f;
        offset2.y -= th2/2.0f;

        pushhudmatrix();
        hudmatrix.scale(hudscorescale, hudscorescale, 1);
        flushhudmatrix();

        int color = hudscoreplayercolour, color2 = hudscoreenemycolour;
        if(!best) swap(color, color2);

        draw_text(buf, int(offset.x), int(offset.y), (color>>16)&0xFF, (color>>8)&0xFF, color&0xFF, hudscorealpha);
        if(score2 > INT_MIN) draw_text(buf2, int(offset2.x), int(offset2.y), (color2>>16)&0xFF, (color2>>8)&0xFF, color2&0xFF, hudscorealpha);

        pophudmatrix();
    }
}

