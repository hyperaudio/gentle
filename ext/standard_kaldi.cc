// Escape hatch from Kaldi.
//
// Use standard in/out trickery to get control flow into other languages.

// Based on online2-wav-nnet2-latgen-faster

#include <stdlib.h>

#include "online2/online-nnet2-decoding.h"
#include "online2/onlinebin-util.h"
#include "online2/online-timing.h"
#include "online2/online-endpoint.h"
#include "fstext/fstext-lib.h"
#include "lat/lattice-functions.h"
#include "lat/word-align-lattice.h"
#include "hmm/hmm-utils.h"
#include "thread/kaldi-thread.h"

const int arate = 8000;

void usage() {
  fprintf(stderr, "usage: standard_kaldi nnet_dir hclg_path proto_lang_dir\n");
}

void ConfigFeatureInfo(kaldi::OnlineNnet2FeaturePipelineInfo& info,
  std::string ivector_model_dir) {
  // online_nnet2_decoding.conf
  info.feature_type = "mfcc";

  // ivector_extractor.conf
  info.use_ivectors = true;
  ReadKaldiObject(
    ivector_model_dir + "/final.mat",
    &info.ivector_extractor_info.lda_mat);
  ReadKaldiObject(
    ivector_model_dir + "/global_cmvn.stats",
    &info.ivector_extractor_info.global_cmvn_stats);
  ReadKaldiObject(
    ivector_model_dir + "/final.dubm",
    &info.ivector_extractor_info.diag_ubm);
  ReadKaldiObject(
    ivector_model_dir + "/final.ie",
    &info.ivector_extractor_info.extractor);
  info.ivector_extractor_info.greedy_ivector_extractor = true;
  info.ivector_extractor_info.ivector_period = 10;
  info.ivector_extractor_info.max_count = 0.0;
  info.ivector_extractor_info.max_remembered_frames = 1000;
  info.ivector_extractor_info.min_post = 0.025;
  info.ivector_extractor_info.num_cg_iters = 15;
  info.ivector_extractor_info.num_gselect = 5;
  info.ivector_extractor_info.posterior_scale = 0.1;
  info.ivector_extractor_info.use_most_recent_ivector = true;

  // splice.conf
  info.ivector_extractor_info.splice_opts.left_context = 3;
  info.ivector_extractor_info.splice_opts.right_context = 3;

  // mfcc.conf
  info.mfcc_opts.frame_opts.samp_freq = arate;
  info.mfcc_opts.use_energy = false;

  info.ivector_extractor_info.Check();
}

void ConfigDecoding(kaldi::OnlineNnet2DecodingConfig &config) {
  config.decodable_opts.acoustic_scale = 0.1;
  config.decoder_opts.lattice_beam = 6.0;
  config.decoder_opts.beam = 15.0;
  config.decoder_opts.max_active = 7000;
}

void ConfigEndpoint(kaldi::OnlineEndpointConfig &config) {
  config.silence_phones = "1:2:3:4:5:6:7:8:9:10:11:12:13:14:15:16:17:18:19:20";
}

int main(int argc, char *argv[]) {
  using namespace kaldi;
  using namespace fst;

  if (argc != 4) {
    usage();
    return EXIT_FAILURE;
  }

  const string nnet_dir = argv[1];
  const string fst_rxfilename = argv[2];
  const string proto_lang_dir = argv[3];

  const string ivector_model_dir = nnet_dir + "/ivector_extractor";
  const string nnet2_rxfilename = proto_lang_dir + "/modeldir/final.mdl";
  const string word_syms_rxfilename = proto_lang_dir + "/langdir/words.txt";
  const string phone_syms_rxfilename = proto_lang_dir + "/langdir/phones.txt";  
  const string word_boundary_filename = proto_lang_dir + "/langdir/phones/word_boundary.int";

  setbuf(stdout, NULL);

  std::cerr << "Loading...\n";

  OnlineNnet2FeaturePipelineInfo feature_info;
  ConfigFeatureInfo(feature_info, ivector_model_dir);
  OnlineNnet2DecodingConfig nnet2_decoding_config;
  ConfigDecoding(nnet2_decoding_config);
  OnlineEndpointConfig endpoint_config;
  ConfigEndpoint(endpoint_config);


  WordBoundaryInfoNewOpts opts; // use default opts
  WordBoundaryInfo word_boundary_info(opts, word_boundary_filename);
  

  BaseFloat frame_shift = feature_info.FrameShiftInSeconds();
  fprintf(stderr, "Frame shift is %f secs.\n", frame_shift);
      
  TransitionModel trans_model;

  nnet2::AmNnet nnet;
  {
    bool binary;
    Input ki(nnet2_rxfilename, &binary);
    trans_model.Read(ki.Stream(), binary);
    nnet.Read(ki.Stream(), binary);
  }

  // This one is much slower than the others.
  fst::Fst<fst::StdArc> *decode_fst = ReadFstKaldi(fst_rxfilename);

    
  fst::SymbolTable *word_syms = fst::SymbolTable::ReadText(word_syms_rxfilename);
  fst::SymbolTable *phone_syms = fst::SymbolTable::ReadText(phone_syms_rxfilename);  

  std::cerr << "Loaded!\n";

  OnlineSilenceWeighting silence_weighting(
                                           trans_model,
                                           feature_info.silence_weighting_config);

  std::unique_ptr<OnlineIvectorExtractorAdaptationState> adaptation_state;
  std::unique_ptr<OnlineNnet2FeaturePipeline> feature_pipeline;
  std::unique_ptr<SingleUtteranceNnet2Decoder> decoder;

  auto reset_decoder = [&]() {
    // Reset the decoding pipeline.
    feature_pipeline.reset(new OnlineNnet2FeaturePipeline(feature_info));
    OnlineIvectorExtractorAdaptationState adaptation_state(
      feature_info.ivector_extractor_info);
    feature_pipeline->SetAdaptationState(adaptation_state);
    decoder.reset(new SingleUtteranceNnet2Decoder(
      nnet2_decoding_config,
      trans_model,
      nnet,
      *decode_fst,
      // TODO(maxahawkins): does this take ownership?
      feature_pipeline.get()));
  };
  reset_decoder();
  
  char cmd[1024];

  while(fgets(cmd, sizeof(cmd), stdin)) {

    if(strcmp(cmd,"stop\n") == 0) {
      // Quit the program.
      break;
    }
    
    else if(strcmp(cmd,"reset\n") == 0) {
      // Reset all decoding state.
      //
      // =Reply=
      // 1. No reply
      reset_decoder();
    }
    else if(strcmp(cmd,"push-chunk\n") == 0) {
      // Add a chunk of audio to the decoding pipeline.
      //
      // =Request=
      // 1. chunk size in bytes (as ascii string)
      // 2. newline
      // 3. binary data as signed 16bit integer pcm
      // =Reply=
      // 1. "ok\n" upon completion
      {
        char chunk_len_str[100];
        fgets(chunk_len_str, sizeof(chunk_len_str), stdin);
        int chunk_len = atoi(chunk_len_str);

        std::vector<char> audio_chunk(chunk_len, 0);
        std::cin.read(&audio_chunk[0], chunk_len);

        int sample_count = chunk_len / 2;

        Vector<BaseFloat> wave_part(sample_count);
        for (int i = 0; i < sample_count; i++) {
          int16_t sample = *reinterpret_cast<int16_t*>(&audio_chunk[i * 2]);
          wave_part(i) = sample;
        }

        feature_pipeline->AcceptWaveform(arate, wave_part);

        // What does this do?
        std::vector<std::pair<int32, BaseFloat> > delta_weights;
        if (silence_weighting.Active()) {
          silence_weighting.ComputeCurrentTraceback(decoder->Decoder());
          silence_weighting.GetDeltaWeights(feature_pipeline->NumFramesReady(),
                                            &delta_weights);
          feature_pipeline->UpdateFrameWeights(delta_weights);
        }

      
        decoder->AdvanceDecoding();

        fprintf(stdout, "ok\n");
      }
    }
    else if(strcmp(cmd,"get-partial\n") == 0) {
      // Dump the provisional (non-word-aligned) transcript for
      // the current lattice.
      //
      // =Reply=
      // 1. One line containing every word in the current lattice
      if (decoder->NumFramesDecoded() == 0) {
        continue;
      }

      Lattice lat;
      decoder->GetBestPath(false, &lat);

      // Let's see what words are in here..

      std::vector<int32> words;
      std::vector<int32> alignment;
      LatticeWeight weight;
      GetLinearSymbolSequence(lat, &alignment, &words, &weight);

      std::stringstream sentence;
      for (size_t i = 0; i < words.size(); i++) {
        std::string s = word_syms->Find(words[i]);
        if (i > 0) {
          sentence << " ";
        }
        sentence << s;
      }
      fprintf(stdout, "%s\n", sentence.str().c_str());
    }
    else if(strcmp(cmd,"get-final\n") == 0) {
      // Dump the final, phone-aligned transcript for the
      // current lattice.
      //
      // =Reply=
      // 1. "phone: / duration:" for every phoneme
      // 2. "word: / start: / duration:" for every word
      // 3. "done with words\n" on completion
      if (decoder->NumFramesDecoded() == 0) {
        fprintf(stdout, "done with words\n");
        continue;
      }

      decoder->FinalizeDecoding();

      Lattice lat;
      decoder->GetBestPath(true, &lat);
      CompactLattice clat;
      ConvertLattice(lat, &clat);

      // Compute prons alignment (see: kaldi/latbin/nbest-to-prons.cc)
      CompactLattice aligned_clat;

      std::vector<int32> words, times, lengths;
      std::vector<std::vector<int32> > prons;
      std::vector<std::vector<int32> > phone_lengths;
      
      WordAlignLattice(clat, trans_model, word_boundary_info, 0, &aligned_clat);

      CompactLatticeToWordProns(trans_model, clat, &words, &times, &lengths,
                                &prons, &phone_lengths);

      for (size_t i = 0; i < words.size(); i++) {
        if (words[i] == 0)  {
          // Don't output anything for <eps> links, which correspond to silence....
          continue;
        }
        fprintf(stdout, "word: %s / start: %f / duration: %f\n",
                word_syms->Find(words[i]).c_str(),
                times[i] * frame_shift,
                lengths[i] * frame_shift);

        // Print out the phonemes for this word
        for(size_t j=0; j<phone_lengths[i].size(); j++) {
          fprintf(stdout, "phone: %s / duration: %f\n",
                  phone_syms->Find(prons[i][j]).c_str(),
                  phone_lengths[i][j] * frame_shift);
        }
      }

      fprintf(stdout, "done with words\n");
    } else {
      fprintf(stdout, "unknown command\n");
    }
  }


  std::cerr << "Goodbye.\n";  
  return 0;
}