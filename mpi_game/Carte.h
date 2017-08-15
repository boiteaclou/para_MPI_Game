#ifndef CARTE_H
#define CARTE_H
#include <vector>
#include "connecteur.h"
#include <mpi.h>
#include "mpi_driver.h"
#include "Action.h"
#include "Update.h"
#include <iostream>
#include "Base.h"
#include <algorithm>
#include <thread>

using namespace std;

namespace carte
{
    using datatype = char;
    using action_datatype = int;

    const size_t MAX_QUEUE_SIZE = 10;
    

    class Scene
    {
        struct update_pack { int pos; char val; };
        using update_datatype = update_pack;
        using update_metatype = int;
        using update_meta_stream_t = updateStream<out_stream, update_metatype, 1>;
        using update_data_stream_t = updateStream<out_stream, update_datatype, 1>;



        int nb_actors;
        mpi_interface::signal_handle end_o_game_signal;
        unsigned int nb_rat = 0, nb_chasseurs = 0, nb_fromages = 0;
        std::vector<datatype> grille; int width; int height;
        unsigned int present_rats;
        
        update_meta_stream_t update_m_stream;
        update_data_stream_t update_d_stream;
        

        void countGridElements()
        {
            for (size_t i = 0; i < grille.size(); ++i)
            {
                if (grille[i] == 'C') {
                    nb_chasseurs++; actor_pos.push_back(i);
                }
                else if (grille[i] == 'R') {
                    nb_rat++; actor_pos.push_front(i);
                }
                else if (grille[i] == 'F') nb_fromages++;
            }
            assert(nb_rat + nb_chasseurs == nb_actors);
            present_rats = nb_rat;
        }

        std::vector<update_pack> updates;

    public:

        std::vector<char> actor_roles;
        std::deque<unsigned int> actor_pos;

        char grid(int i) { return grille[i]; }

        void printMap()
        {
            int cpt = 0;
            for (size_t i = 0; i < grille.size(); i++)
            {
                if (cpt == width)
                {
                    cpt = 0;
                    cout << "\n" << endl;
                }
                cout << grille[i] << endl;
                cpt++;
            }
        }

        Scene(std::vector<datatype>&& grille, int width, int height, MPI_Win* end_o_game_w, int nb_actors) : nb_actors{nb_actors}, end_o_game_signal { end_o_game_w }, nb_rat{ 0 }, nb_chasseurs{ 0 }, nb_fromages{ 0 }, 
                                                                                                             grille{ grille }, width{ width }, height{ height },
                                                                                                             update_m_stream(mpi_driver::make_mpi_context(0, 0, MPI_COMM_WORLD, MPI_INT)),
                                                                                                             update_d_stream(mpi_driver::make_mpi_context(0, 0, MPI_COMM_WORLD))
        {
            update_m_stream.context.count = 2;
            countGridElements();
            actor_roles.resize(nb_actors);
        }

        void endGame()
        {
            for (unsigned int i = 1; i < nb_chasseurs + nb_rat; ++i) end_o_game_signal.put<MPI_C_BOOL>(true, i);
        }

        void removeRat(int caller)
        {
            bool end{ true };
            if (grille[actor_pos[caller]] == 'R') grille[actor_pos[caller]] = ' ';
            --present_rats; if (present_rats == 0) endGame();
            end_o_game_signal.put<MPI_C_BOOL>(true, caller);
        }

        void eatCheese(int position)
        {
            if (grille[position] == 'F') grille[position] = ' ';
            --nb_fromages; if (nb_fromages == 0) endGame();
        }

        void assignRoles(unsigned int nb_actor_procs)
        {
            mpi_interface::mpi_main_connector<char> connector;
            auto context = mpi_driver::make_mpi_context(MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, MPI_CHAR);
            context.count = 1;
            for (unsigned int i = 1; i <= nb_actor_procs; ++i)
            {
                char chasseur = '1', rat = '0';
                char me = (i > nb_rat) ? 'C' : 'R';
                actor_roles[i] = me;
                std::cout << "map initializing actor nb " << i << " . It a " << me << "." << std::endl;
                context.target = i;
                if(i > nb_rat) connector.request<canal_direction::_send>(context, &rat);
                else connector.request<canal_direction::_send>(context, &chasseur);
            }
        }

        void triggerMeow(int actor) 
        {
            std::vector<unsigned int> pos;
            int i_a = actor_pos[actor] % width, j_a = actor_pos[actor] / width;
            for (int i = std::max(0, i_a - 4); i < std::min(width, i_a + 3); ++i) {
                for (int j = std::max(0, j_a - 4); j < std::min(height, j_a + 3); ++j) {
                    if (grille[i * width + j] == 'R')  pos.push_back(i * width + j);
                }
            }
            int meta[2] = { -1, 0 };
            std::for_each(pos.begin(), pos.end(), [&](unsigned int pos){
                update_m_stream.context.target = *(std::find(actor_pos.begin(), actor_pos.end(), pos));
                update_m_stream << &meta[0];
            });
        }

        void propagateUpdate()
        {
            int meta[2] = { 1 , int(updates.size()) };
            update_d_stream.context.count = updates.size();
            update_pack* buff = static_cast<update_pack*>(malloc(sizeof(update_pack) * updates.size()));
            std::cout << "SCEN | creating update" << std::endl;
            for (int i = 0; i < updates.size(); ++i)
            {
                buff[i] = updates[i];
            }
            std::cout << "SCEN | end creating update" << std::endl;
            std::cout << "SCEN | gotta send meta " << meta[0] << " " << meta[1] << std::endl;
            for (int i = 1; i <= nb_chasseurs + nb_rat; ++i) {
                std::cout << "SCEN | sending to " << i << std::endl;
                update_m_stream.context.target = i;
                update_d_stream.context.target = i;
                update_m_stream << &meta[0];
                if(meta[1] > 0) update_d_stream << &buff[0];
            }
            free(buff);
        }

        void updateSelf(std::pair<int,action_datatype> data)
        {
            std::swap(grille[actor_pos[data.first]], grille[data.second]);
            update_pack pack_1, pack_2; 
            pack_1.pos = actor_pos[data.first]; pack_1.val = grille[actor_pos[data.first]];
            pack_2.pos = data.second; grille[data.second];
            actor_pos[data.first] = data.second;
            std::cout << "updating grille" << std::endl;
            updates.push_back(pack_1);
            updates.push_back(pack_2);
        }
    };

    class Juge
    {
        int cpt = 0;
        bool in_function = true;
        
        int nb_actors;
        Scene* scene_ptr;

        actionStream<a_in_stream, action_datatype, 1> action_stream;

    public:
        Juge(Scene* scene, int nb_actors) : nb_actors{ nb_actors }, scene_ptr{ scene },
                                            action_stream(mpi_driver::make_mpi_context(0, 0, MPI_COMM_WORLD, MPI_INT))
        {
            action_stream.context.count = 1;
            action_stream.context.target = MPI_ANY_SOURCE;
        }

        void listen()
        {
            std::cout << "JUGE | listening" << std::endl;
            int cpt = nb_actors, cptg = 0;
            std::vector<request_t<action_datatype*>>::iterator actions;
            while (cptg < 2 * nb_actors && in_function && action_stream >> actions)
            {
                if (cpt != 0) {
                    --cpt;
                    int caller = 1000;
                    std::cout << "JUGE | processing an action" << std::endl;
                    action_datatype* action = action_stream.unpack(actions, caller);
                    processAction(action, caller);
                    cptg++;
                }
                else
                {
                    std::this_thread::sleep_for(1000ms);
                    cpt = nb_actors;
                    scene_ptr->propagateUpdate();
                }
            }
            scene_ptr->endGame();
        }

        void fakeListen()
        {
            std::vector<request_t<action_datatype*>>::iterator actions;
            std::cout << "JUGE | setting cannals" << std::endl;
            while (cpt < 40 && in_function && action_stream >> actions)
            {
                int caller = 0;
                processAction(action_stream.unpack(actions, caller), caller);
                cpt++;
            }
            std::this_thread::sleep_for(1000ms);
            scene_ptr->endGame();
        }

        void processFake(action_datatype* action, int caller)
        {
            std::cout << "Juge received movement " << action << " from " << caller << std::endl;
        }

        bool processAction(action_datatype* action, int caller) const
        {
            std::cout << "JUGE | received action " << *action << " from " << caller << std::endl;
            char cnd;
            if (*action == -1) scene_ptr->triggerMeow(caller);
            else {
                if (scene_ptr->grid(*action) == 'M') return false;
                if (scene_ptr->grid(*action) == '+' && scene_ptr->actor_roles[caller] == 'R') {
                    scene_ptr->removeRat(caller);
                    return false;
                }
                if (scene_ptr->grid(*action) == 'F' && scene_ptr->actor_roles[caller] == 'R') {
                    scene_ptr->eatCheese(scene_ptr->actor_pos[caller]);
                }
                if (scene_ptr->grid(*action) == 'R' && scene_ptr->actor_roles[caller] == 'C') {
                    scene_ptr->removeRat(caller);
                }
                scene_ptr->updateSelf(std::make_pair(caller, *action));
            }
        }

        void close() { in_function = false; }
    };

    class Carte
    {
        Scene scene;
        Juge juge;

    public:

        void initializeActors(unsigned int nb_actor_procs)
        {
            scene.assignRoles(nb_actor_procs);
        }

        Carte() = delete;
        Carte(unsigned int nb_actor_procs, std::vector<char> grille, int width, int height, MPI_Win* end_o_game_w) : scene{ std::move(grille), width, height, end_o_game_w , nb_actor_procs }, juge{ &scene, nb_actor_procs }
        {
            
        }

        void startGame()
        {
            juge.listen();
        }

        void fakeStartGame()
        {
            juge.listen();
        }

    };
}

#endif