#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include "config.hpp"
#include "open_jtalk.hpp"
/* Main headers */
#include "mecab.h"
#include "njd.h"
#include "jpcommon.h"
#include "HTS_engine.h"

/* Sub headers */
#include "text2mecab.h"
#include "mecab2njd.h"
#include "njd_set_pronunciation.h"
#include "njd_set_digit.h"
#include "njd_set_accent_phrase.h"
#include "njd_set_accent_type.h"
#include "njd_set_unvoiced_vowel.h"
#include "njd_set_long_vowel.h"
#include "njd2jpcommon.h"

#include "../../sound-mixing-proto/index.pb.h"
#include "../../sound-mixing-proto/index.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <condition_variable>
#include <filesystem>
#include <opus.h>
namespace fs = std::filesystem;
TTS::TTS()
{
   Mecab_initialize(&open_jtalk.mecab);
   NJD_initialize(&open_jtalk.njd);
   JPCommon_initialize(&open_jtalk.jpcommon);
   HTS_Engine_initialize(&open_jtalk.engine);
}

int TTS::synthesis(const char *txt, const char *oggfn, FILE *opusfp, FILE *wavfp,
                   FILE *logfp)
{
   int result = 0;
   char buff[MAXBUFLEN];

   text2mecab(buff, txt);
   Mecab_analysis(&open_jtalk.mecab, buff);
   mecab2njd(&open_jtalk.njd, Mecab_get_feature(&open_jtalk.mecab),
             Mecab_get_size(&open_jtalk.mecab));
   njd_set_pronunciation(&open_jtalk.njd);
   njd_set_digit(&open_jtalk.njd);
   njd_set_accent_phrase(&open_jtalk.njd);
   njd_set_accent_type(&open_jtalk.njd);
   njd_set_unvoiced_vowel(&open_jtalk.njd);
   njd_set_long_vowel(&open_jtalk.njd);
   njd2jpcommon(&open_jtalk.jpcommon, &open_jtalk.njd);
   JPCommon_make_label(&open_jtalk.jpcommon);
   if (JPCommon_get_label_size(&open_jtalk.jpcommon) > 2)
   {
      if (HTS_Engine_synthesize_from_strings(&open_jtalk.engine, JPCommon_get_label_feature(&open_jtalk.jpcommon),
                                             JPCommon_get_label_size(&open_jtalk.jpcommon)) == TRUE)
      {
         result = 1;
      }
      if (wavfp != NULL)
      {
         HTS_Engine_save_riff(&open_jtalk.engine, wavfp);
      }
      if (oggfn != NULL)
      {
         HTS_Engine_save_ogg(&open_jtalk.engine, oggfn);
      }
      if (opusfp != NULL)
      {
         HTS_Engine_save_opus(&open_jtalk.engine, opusfp);
      }
      if (logfp != NULL)
      {
         fprintf(logfp, "[Text analysis result]\n");
         NJD_fprint(&open_jtalk.njd, logfp);
         fprintf(logfp, "\n[Output label]\n");
         HTS_Engine_save_label(&open_jtalk.engine, logfp);
         fprintf(logfp, "\n");
         HTS_Engine_save_information(&open_jtalk.engine, logfp);
      }
      HTS_Engine_refresh(&open_jtalk.engine);
   }
   JPCommon_refresh(&open_jtalk.jpcommon);
   NJD_refresh(&open_jtalk.njd);
   Mecab_refresh(&open_jtalk.mecab);

   return result;
}
void TTS::clear()
{
   Mecab_clear(&open_jtalk.mecab);
   NJD_clear(&open_jtalk.njd);
   JPCommon_clear(&open_jtalk.jpcommon);
   HTS_Engine_clear(&open_jtalk.engine);
}
int TTS::load(char *dn_mecab, char *fn_voice)
{

   {
      if (Mecab_load(&open_jtalk.mecab, dn_mecab) != TRUE)
      {
         clear();
         return 0;
      }
      if (HTS_Engine_load(&open_jtalk.engine, &fn_voice, 1) != TRUE)
      {
         clear();
         return 0;
      }
      if (strcmp(HTS_Engine_get_fullcontext_label_format(&open_jtalk.engine), "HTS_TTS_JPN") != 0)
      {
         clear();
         return 0;
      }
      return 1;
   }
}

//fd is pipe etc
static bool FdToWriter(int fd, ::grpc::ServerWriter<::ChunkedData> *writer)
{
   static const constexpr int BUFLEN = 4096;
   char buf[BUFLEN];
   int size;

   do
   {
      errno = 0;
      size = read(fd, buf, BUFLEN);
      if (size < 0)
      {
         if (errno == EAGAIN || errno == EWOULDBLOCK)
         {
            errno = 0;
            break;
         }
         fprintf(stderr, "read error!%s!\n", strerror(errno));
         return true;
      }
      if (size == 0)
      {
         return true;
      }
      ChunkedData data;
      std::string str(buf, size);
      data.set_data(str);

      if (!writer->Write(data))
      {
         fprintf(stderr, "writer#Write Failed!\n");
         return true;
      }

   } while (size >= BUFLEN);
   return false;
}
static void setnonblocking(int sock)
{
   int flag = fcntl(sock, F_GETFL, 0);
   fcntl(sock, F_SETFL, flag | O_NONBLOCK);
}
struct FdAndWriter
{
   int fd;
   ::grpc::ServerWriter<::ChunkedData> *writer;
   std::condition_variable cond;
   std::mutex mtx;
   bool exiting;
};
class ServiceImpl final : public Mixer::Service
{
private:
   int _epoll_fd;
   std::unordered_map<std::string, std::string> _conf;

public:
   ServiceImpl(int epoll_fd, std::unordered_map<std::string, std::string> conf) : _epoll_fd(epoll_fd), _conf(conf)
   {
   }
   ::grpc::Status mixing(::grpc::ServerContext *context, const ::RequestVoiceMixing *request, ::grpc::ServerWriter<::ChunkedData> *writer) override
   {

      /* dictionary directory */
      char *dn_dict = std::getenv("OPEN_JTALK_DIC");

      /* HTS voice */
      auto fn_voice_itr = _conf.find(request->htsvoice());
      if (fn_voice_itr == _conf.end())
      {
         return ::grpc::Status::CANCELLED;
      }
      std::vector<char> fn_voice_vec(fn_voice_itr->second.c_str(), fn_voice_itr->second.c_str() + fn_voice_itr->second.size() + 1);
      char *fn_voice = fn_voice_vec.data();
      /* input text file name */
      FILE *txtfp = stdin;
      char *txtfn = NULL;

      /* output file pointers */
      FILE *wavfp = NULL;
      int opusfd[2];
      if (pipe2(opusfd, O_DIRECT) == -1)
      {
         perror("pipe");
         return ::grpc::Status::CANCELLED;
      }

      FILE *opusfp = fdopen(opusfd[1], "w");
      setvbuf(opusfp, NULL, _IONBF, 0);
      const char *oggfn = NULL;
      FILE *logfp = NULL;
      TTS tts;

      if (tts.load(dn_dict, fn_voice) != TRUE)
      {
         fprintf(stderr, "Error: Dictionary or HTS voice cannot be loaded.\n");
         tts.clear();
      }

      tts.set_speed(request->speed());
      tts.set_volume(request->volume());
      tts.add_half_tone(request->tone());
      tts.set_msd_threshold(1, request->threshold());
      if (request->allpass() >= 0)
      {
         tts.set_alpha(request->allpass());
      }
      tts.set_gv_weight(1, request->intone());
      struct epoll_event ev;
      ev.events = EPOLLIN;
      setnonblocking(opusfd[0]);
      auto ptr = std::unique_ptr<FdAndWriter>(new FdAndWriter{opusfd[0], writer});
      FdAndWriter *pptr = ptr.get();
      ev.data.ptr = pptr;
      if (epoll_ctl(_epoll_fd, EPOLL_CTL_ADD, ptr->fd, &ev) == -1)
      {
         perror("epoll_ctl: opusfp");
         return ::grpc::Status::CANCELLED;
      }
      writer->SendInitialMetadata();
      auto text = request->text().c_str();
      if (tts.synthesis(text, oggfn, opusfp, wavfp, logfp) != TRUE)
      {
         fprintf(stderr, "Error: waveform cannot be synthesized.\n");
         tts.clear();
         return ::grpc::Status::CANCELLED;
      }

      /* free memory */
      tts.clear();

      /* close files */
      if (txtfn != NULL)
         fclose(txtfp);
      if (wavfp != NULL)
         fclose(wavfp);
      if (logfp != NULL)
         fclose(logfp);
      if (opusfp != NULL)
         fclose(opusfp);
      std::unique_lock<std::mutex> lk(ptr->mtx);
      {
         ptr->cond.wait(lk, [pptr] { return pptr->exiting; });
      }
      return ::grpc::Status::OK;
   }
};
class Main
{
   static const constexpr int MAX_EVENTS = 10;

private:
   std::string readAll(fs::path fname)
   {
      std::ifstream t(fname);
      std::string str((std::istreambuf_iterator<char>(t)),
                      std::istreambuf_iterator<char>());
      return str;
   }
   std::shared_ptr<grpc::ServerCredentials> makeCredentials(fs::path keys)
   {
      grpc::SslServerCredentialsOptions options;
      options.client_certificate_request = grpc_ssl_client_certificate_request_type ::GRPC_SSL_REQUEST_CLIENT_CERTIFICATE_AND_VERIFY;
      auto serverKey = keys;
      serverKey /= "server.key";
      auto serverCrt = keys;
      serverCrt /= "server.crt";
      auto pair = grpc::SslServerCredentialsOptions::PemKeyCertPair{
          readAll(serverKey), readAll(serverCrt)};
      options.pem_key_cert_pairs = {pair};
      auto caCrt = keys;
      caCrt /= "ca.crt";
      options.pem_root_certs = readAll(caCrt);
      auto r = grpc::SslServerCredentials(options);
      return r;
   }

public:
   void run()
   {
      auto conf = config("/config");
      fprintf(stderr, "config read!\n");

      auto epollfd = epoll_create1(0);
      if (epollfd == -1)
      {
         perror("epoll_create1");
         exit(EXIT_FAILURE);
      }
      std::string server_address("0.0.0.0:50051");
      ServiceImpl service(epollfd, conf);
      grpc_impl::ServerBuilder builder;
      auto keys = std::getenv("GUJ_MIXER_KEYS");
      auto credentials = keys == NULL ? grpc::InsecureServerCredentials() : makeCredentials(keys);
      builder.AddListeningPort(server_address, credentials);
      builder.RegisterService(&service);
      std::unique_ptr<grpc_impl::Server> server(builder.BuildAndStart());
      std::cout << "Server listening on " << server_address << std::endl;
      struct epoll_event events[MAX_EVENTS];
      for (;;)
      {
         int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);

         if (nfds == -1)
         {
            perror("epoll_pwait");
         }

         for (int n = 0; n < nfds; ++n)
         {
            FdAndWriter *ptr = reinterpret_cast<FdAndWriter *>(events[n].data.ptr);
            if ((events[n].events & EPOLLERR) != 0)
            {
               fprintf(stderr, "EPOLLERR!\n");
               epoll_ctl(epollfd, EPOLL_CTL_DEL, ptr->fd, NULL);
               close(ptr->fd);
               continue;
            }
            if (FdToWriter(ptr->fd, ptr->writer))
            {
               epoll_ctl(epollfd, EPOLL_CTL_DEL, ptr->fd, NULL);
               close(ptr->fd);
               {
                  std::lock_guard<std::mutex> lock(ptr->mtx);
                  ptr->exiting = true;
               }
               ptr->cond.notify_all();
            }
         }
      }
      server->Wait();
   }
};
int main()
{
   Main m;
   m.run();
}