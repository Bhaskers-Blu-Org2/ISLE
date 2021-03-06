// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include "trainer.h"

namespace ISLE
{
    ISLETrainer::ISLETrainer(
        const word_id_t		vocab_size_,
        const doc_id_t		num_docs_,
        const offset_t		max_entries_,
        const doc_id_t		num_topics_,
        const bool          tf_idf_,
        const bool			flag_sample_docs_,
        const FPTYPE		sample_rate_,
        const data_ingest	how_data_loaded_,
        const std::string&	input_file_,
        const std::string&	vocab_file_,
        const std::string&	output_path_base_,
        const bool			flag_construct_edge_topics_,
        const int			max_edge_topics_,
        const bool			flag_compute_log_combinatorial_,
        const bool			flag_compute_distinct_top_five_sets_,
        const bool			flag_compute_avg_coherence_,
        const bool			flag_print_doctopic_,
        const bool          flag_print_top_two_topics_)
        :
        vocab_size(vocab_size_),
        num_docs(num_docs_),
        avg_doc_sz(0),
        max_entries(max_entries_),
        num_topics(num_topics_),
        tf_idf(tf_idf_),
        how_data_loaded(how_data_loaded_),
        input_file(input_file_),
        vocab_file(vocab_file_),
        output_path_base(output_path_base_),
        flag_sample_docs(flag_sample_docs_),
        sample_rate(sample_rate_),
        normalized_vals_CSR(NULL),
        cols_CSR(NULL),
        offsets_CSR(NULL),
        flag_construct_edge_topics(flag_construct_edge_topics_),
        max_edge_topics(max_edge_topics_),
        flag_compute_log_combinatorial(flag_compute_log_combinatorial_),
        flag_compute_distinct_top_five_sets(flag_compute_distinct_top_five_sets_),
        flag_compute_avg_coherence(flag_compute_avg_coherence_),
        flag_print_doctopic(flag_print_doctopic_),
        flag_print_top_two_topics(flag_print_top_two_topics_),
        is_training_complete(false)
    {
        //
        // Check size alignments
        //
        if (sizeof(MKL_INT) == 4)
        {
            assert(num_docs < 0x7fffffff && vocab_size < 0x7fffffff);
            //assert(((uint64_t)num_docs)*((uint64_t)vocab_size) < 0x7fffffff);
#ifdef MKL_USE_DNSCSR
            assert(max_entries < 0x7fffffff);
#endif
        }

        //
        // Initialize  is_log_file_open directories, vocablist, and timer
        //
        log_dir = log_dir_name(num_topics, output_path_base, flag_sample_docs, sample_rate, tf_idf);
        create_dir(log_dir);

        out_log = new LogUtils(log_dir);
        timer = new Timer(log_dir);

        is_data_loaded = false;

        EdgeModel = NULL;

        if (how_data_loaded == data_ingest::FILE_DATA_LOAD)
            load_data_from_file();
        if (how_data_loaded == data_ingest::PREPROCESSED_DATA_LOAD)
            load_preprocessed_data_from_file();
    }

    ISLETrainer::~ISLETrainer()
    {
        //
        // Clean up data matrices
        //
        delete A_sp;
        delete B_fl_CSC;
        delete Model;
        delete EdgeModel;

        //
        // Clean up CSR data
        //
        if (normalized_vals_CSR != NULL)
            delete[] normalized_vals_CSR;
        if (cols_CSR != NULL)
            delete[] cols_CSR;
        if (offsets_CSR != NULL)
            delete[] offsets_CSR;

        //
        // Cleanup and exit
        //
        delete[] catchword_thresholds;
        delete[] centers;
        delete[] closest_docs;
        delete[] catchwords;
        delete[] topwords;

        timer->next_time_secs("Cleaning up");
        timer->total_time_secs("TVSD");

        delete timer;
        delete out_log;
    }

    //
    // Load file data
    // Load a vocabulary list from file
    // convert file data to sparse data matrix 
    //
    void ISLETrainer::load_data_from_file()
    {
        assert(how_data_loaded == data_ingest::FILE_DATA_LOAD);

        DocWordEntriesReader reader(entries);
        reader.read_from_file(input_file, max_entries);
        std::ostringstream data_size_stream;
        data_size_stream
            << "\n<<<<<<<<<<<<\t" << input_file << "\t>>>>>>>>>>>>\n\n"
            << std::setfill('.') << std::setw(10) << std::left
            << std::setw(15) << std::left << "#Entries" << entries.size() << "\n"
            << std::setw(15) << std::left << "#Words" << vocab_size << "\n"
            << std::setw(15) << std::left << "#Docs" << num_docs << "\n"
            << std::setw(15) << std::left << "#Topics" << num_topics << "\n"
            << std::setw(15) << std::left << "TF-IDF" << tf_idf << "\n"
            << std::setw(15) << std::left << "Sampling?" << flag_sample_docs << "\n"
            << std::setw(15) << std::left << "Sample rate" << sample_rate << "\n"
            << std::setw(15) << std::left << "Edge topics?" << flag_construct_edge_topics << "\n"
            << std::setw(15) << std::left << "#Edge topics" << max_edge_topics << std::endl;
        out_log->print_stringstream(data_size_stream);
        timer->next_time_secs("Reading file Entries");

        finalize_data();

        if (flag_compute_log_combinatorial) print_log_combinatorial();
        if (flag_compute_distinct_top_five_sets) print_distinct_top_five_sets();
    }

    //
    // Load preprocessed file data
    // Load a vocabulary list from file
    // convert file data to sparse data matrix 
    //
    void ISLETrainer::load_preprocessed_data_from_file()
    {
        assert(how_data_loaded == data_ingest::PREPROCESSED_DATA_LOAD);

        std::ifstream info_stream(std::string(input_file) + "_tr.info");
        word_id_t   file_vocab_size;
        doc_id_t    file_num_docs;
        offset_t    file_nnzs;
        FPTYPE      file_avg_doc_sz;
        info_stream >> file_num_docs;
        info_stream >> file_vocab_size;
        info_stream >> file_nnzs;
        info_stream >> file_avg_doc_sz;
        info_stream.close();

        if (vocab_size == 0) vocab_size = file_vocab_size;
        else if (vocab_size != file_vocab_size) {
            std::cerr << "Vocab size information mismatch" << std::endl;
            exit(-1);
        }
        if (num_docs == 0) num_docs = file_num_docs;
        else if (num_docs != file_num_docs) {
            std::cerr << "Num docs information mismatch" << std::endl;
            exit(-1);
        }
        if (max_entries != file_nnzs) {
            std::cerr << "Num entries information mismatch" << std::endl;
            exit(-1);
        }
        if (file_avg_doc_sz > 1.0) avg_doc_sz = file_avg_doc_sz;
        else {
            std::cerr << "Avg doc size error" << std::endl;
            exit(-1);
        }

        std::ostringstream data_size_stream;
        data_size_stream
            << "\n<<<<<<<<<<<<\t" << input_file << "\t>>>>>>>>>>>>\n\n"
            << std::setfill('.') << std::setw(10) << std::left
            << std::setw(15) << std::left << "#Entries" << max_entries << "\n"
            << std::setw(15) << std::left << "#Words" << vocab_size << "\n"
            << std::setw(15) << std::left << "#Docs" << num_docs << "\n"
            << std::setw(15) << std::left << "#Topics" << num_topics << "\n"
            << std::setw(15) << std::left << "Sampling?" << flag_sample_docs << "\n"
            << std::setw(15) << std::left << "Sample rate" << sample_rate << "\n"
            << std::setw(15) << std::left << "Edge topics?" << flag_construct_edge_topics << "\n"
            << std::setw(15) << std::left << "#Edge topics" << max_edge_topics << std::endl;
        out_log->print_stringstream(data_size_stream);

        finalize_data();
        timer->next_time_secs("Reading file Entries");


        if (flag_compute_log_combinatorial) print_log_combinatorial();
        if (flag_compute_distinct_top_five_sets) print_distinct_top_five_sets();
    }

    void ISLETrainer::feed_data(
        const doc_id_t doc,
        const word_id_t *const words,
        const count_t *const counts,
        const offset_t num_words)
    {
        DocWordEntry<count_t> entry;
        for (auto w = 0; w < num_words; ++w) {
            entry.doc = doc;
            entry.word = words[w] - 1;
            entry.count = counts[w];
            entries.push_back(entry);
        }
    }

    //
    // Populate the data matrix and normalize it, clear DataEntries
    //
    void ISLETrainer::finalize_data()
    {
        assert(is_data_loaded == false);

        if (how_data_loaded == data_ingest::FILE_DATA_LOAD) {
            parallel_sort(			// Sort by doc first, and word second.
                entries.begin(), entries.end(),
                [](const auto& l, const auto& r)
            {return (l.doc < r.doc) || (l.doc == r.doc && l.word < r.word); });
            timer->next_time_secs("Sorting entries");

            entries.erase(			// Remove duplicates
                std::unique(entries.begin(), entries.end(),
                    [](const auto& l, const auto& r) {return l.doc == r.doc && l.word == r.word; }),
                entries.end());
            timer->next_time_secs("De-duplicating entries");

            if (num_docs == 0) {
                num_docs = entries.back().doc + 1;
                std::cout << std::setw(20) << std::left << "#Docs(updated)" << num_docs << "\n";
            }
            else
                assert(entries.back().doc < num_docs);
            if (vocab_size == 0) {
                for (auto iter = entries.begin(); iter != entries.end(); ++iter)
                    vocab_size = (vocab_size > iter->word) ? vocab_size : iter->word;
                ++vocab_size;
                std::cout << std::setw(20) << std::left << "#Words(updated)" << vocab_size << "\n";
            }
        }

        if (tf_idf) {
            if (how_data_loaded == data_ingest::PREPROCESSED_DATA_LOAD) {
                std::cerr << "TF-IDF is to be used only for text inputs." << std::endl;
                exit(-1);
            }
           
            std::vector<float> idf(vocab_size+1, 0.0);
            for (auto iter : entries)
                idf[iter.word] += 1.0;
			for (word_id_t w = 1; w <= vocab_size; ++w)
				idf[w] = std::log(((float)num_docs) / idf[w]);
			for (auto iter : entries)
                iter.count = (count_t) std::ceil(idf[iter.word] * (float)iter.count); 
        }

        A_sp = new SparseMatrix<A_TYPE>(vocab_size, num_docs);
        B_fl_CSC = new FPSparseMatrix<FPTYPE>(vocab_size, num_docs);

        catchwords   = new std::vector<word_id_t>[num_topics];
        topwords     = new std::vector<std::pair<word_id_t, FPTYPE> >[num_topics];
        closest_docs = new std::vector<doc_id_t>[num_topics];
        centers      = new FPTYPE[(size_t)num_topics * (size_t)vocab_size];
        Model        = new DenseMatrix<FPTYPE>(vocab_size, num_topics);
        catchword_thresholds = new A_TYPE[(size_t)vocab_size * (size_t)num_topics];
        
        create_vocab_list(vocab_file, vocab_words, vocab_size);

        if (how_data_loaded == data_ingest::FILE_DATA_LOAD) {
            A_sp->populate_CSC(entries);
            timer->next_time_secs("Populating CSC");
            A_sp->normalize_docs(true); // delete unnormalized values
            timer->next_time_secs("Populating CSC");
        }
        else if (how_data_loaded == data_ingest::PREPROCESSED_DATA_LOAD) {
            //
            // Load CSC files
            //
            std::ifstream vals_CSC_stream(std::string(input_file) + "_tr.csr", std::ios::binary | std::ios::in);
            vals_CSC_stream.seekg(0, std::ios::end);
            offset_t vals_CSC_len = vals_CSC_stream.tellg();
            assert(vals_CSC_len == sizeof(FPTYPE) * max_entries);
            vals_CSC_stream.seekg(0, std::ios::beg);
            FPTYPE *normalized_vals_CSC = new FPTYPE[max_entries];
            vals_CSC_stream.read((char*)normalized_vals_CSC, vals_CSC_len);
            vals_CSC_stream.close();

            std::ifstream rows_CSC_stream(std::string(input_file) + "_tr.col", std::ios::binary | std::ios::in);
            rows_CSC_stream.seekg(0, std::ios::end);
            offset_t rows_CSC_len = rows_CSC_stream.tellg();
            assert(rows_CSC_len == sizeof(word_id_t) * max_entries);
            rows_CSC_stream.seekg(0, std::ios::beg);
            word_id_t *rows_CSC = new word_id_t[max_entries];
            rows_CSC_stream.read((char*)rows_CSC, rows_CSC_len);
            rows_CSC_stream.close();

            std::ifstream offsets_CSC_stream(std::string(input_file) + "_tr.off", std::ios::binary | std::ios::in);
            offsets_CSC_stream.seekg(0, std::ios::end);
            offset_t offsets_CSC_len = offsets_CSC_stream.tellg();
            assert(offsets_CSC_len == sizeof(offset_t) * (num_docs + 1));
            offsets_CSC_stream.seekg(0, std::ios::beg);
            offset_t *offsets_CSC = new offset_t[num_docs + 1];
            offsets_CSC_stream.read((char*)offsets_CSC, offsets_CSC_len);
            offsets_CSC_stream.close();

            //
            // Load CSR files
            //
            std::ifstream vals_CSR_stream(std::string(input_file) + ".csr", std::ios::binary | std::ios::in);
            vals_CSR_stream.seekg(0, std::ios::end);
            offset_t vals_CSR_len = vals_CSR_stream.tellg();
            assert(vals_CSR_len == sizeof(FPTYPE) * max_entries);
            vals_CSR_stream.seekg(0, std::ios::beg);
            normalized_vals_CSR = new FPTYPE[max_entries];
            vals_CSR_stream.read((char*)normalized_vals_CSR, vals_CSR_len);
            vals_CSR_stream.close();

            std::ifstream cols_CSR_stream(std::string(input_file) + ".col", std::ios::binary | std::ios::in);
            cols_CSR_stream.seekg(0, std::ios::end);
            offset_t cols_CSR_len = cols_CSR_stream.tellg();
            assert(cols_CSR_len == sizeof(doc_id_t) * max_entries);
            cols_CSR_stream.seekg(0, std::ios::beg);
            cols_CSR = new word_id_t[max_entries];
            cols_CSR_stream.read((char*)cols_CSR, cols_CSR_len);
            cols_CSR_stream.close();

            std::ifstream offsets_CSR_stream(std::string(input_file) + ".off", std::ios::binary | std::ios::in);
            offsets_CSR_stream.seekg(0, std::ios::end);
            offset_t offsets_CSR_len = offsets_CSR_stream.tellg();
            assert(offsets_CSR_len == sizeof(offset_t) * (vocab_size + 1));
            offsets_CSR_stream.seekg(0, std::ios::beg);
            offsets_CSR = new offset_t[num_docs + 1];
            offsets_CSR_stream.read((char*)offsets_CSR, offsets_CSR_len);
            offsets_CSR_stream.close();

            assert(offsets_CSR[vocab_size] == offsets_CSC[num_docs]);

            A_sp->populate_preprocessed_CSC(
                max_entries, avg_doc_sz,
                normalized_vals_CSC, rows_CSC, offsets_CSC);
        } 
        else assert(false);
        is_data_loaded = true;

        if (how_data_loaded == data_ingest::FILE_DATA_LOAD) {
            entries.clear();
            entries.shrink_to_fit(); // Remove allocated memory
            entries.swap(entries);   // force deallocation of memory
        }
    }

    void ISLETrainer::print_log_combinatorial()
    {
        std::vector<FPTYPE> docsLogFact;
        A_sp->compute_log_combinatorial(docsLogFact);
        std::ofstream out_comb_log;
        out_comb_log.open(concat_file_path(log_dir, std::string("LogCombinatorial.txt")));
        for (auto iter = docsLogFact.begin(); iter != docsLogFact.end(); ++iter)
            out_comb_log << *iter << std::endl;
        out_comb_log.close();
        timer->next_time_secs("Print Log Combinatorial");
    }

    //
    // Create a multiset of top 5 words for each doc
    // Count the number of these multi-instances occur more than x times
    //
    void ISLETrainer::print_distinct_top_five_sets()
    {
        std::ostringstream ostr;
        ostr << "Distinct top five sets: "
            << A_sp->count_distint_top_five_words(2) << " "
            << A_sp->count_distint_top_five_words(5) << " "
            << A_sp->count_distint_top_five_words(10) << " "
            << A_sp->count_distint_top_five_words(20) << " "
            << A_sp->count_distint_top_five_words(50) << " "
            << A_sp->count_distint_top_five_words(100) << " "
            << A_sp->count_distint_top_five_words(200) << " "
            << A_sp->count_distint_top_five_words(500) << " "
            << std::endl;
        out_log->print_stringstream(ostr, true);
        timer->next_time_secs("Distinct top-5 words");
    }

    //
    // Computing singular vals of A_sp
    //
    void ISLETrainer::compute_input_svd()
    {
        FPSparseMatrix<FPTYPE> A_fl_CSC(*A_sp, true);
        A_fl_CSC.initialize_for_eigensolver(num_topics);
        timer->next_time_secs("Spectra A_sp init");
        std::vector<FPTYPE> A_sq_svalues;
        A_fl_CSC.compute_Spectra(num_topics, A_sq_svalues);
        out_log->print_string("Frob_Sq(A_sp_fl): " + std::to_string(A_fl_CSC.normalized_frobenius()) + "\n");
        std::ofstream ostr;
        ostr.open(concat_file_path(output_path_base, std::string("A_squared_spectrum.txt")));
        ostr << "Frob_Sq(A_sp_fl): " << A_fl_CSC.normalized_frobenius() << std::endl;
        out_log->print_eigen_data(A_sq_svalues, num_topics);
        A_fl_CSC.cleanup_after_eigensolver();
        timer->next_time_secs("Spectra A_sp computation");
    }

	void ISLETrainer::train()
	{
		//
		// Threshold
		//
		std::vector<A_TYPE> thresholds(vocab_size, 0);
		auto freqs = new std::vector<A_TYPE>[vocab_size];
		offset_t new_nnzs = 0;
		if (how_data_loaded == data_ingest::FILE_DATA_LOAD) {
			A_sp->list_word_freqs_by_sorting(freqs);
			new_nnzs = A_sp->compute_thresholds(0, vocab_size, freqs, thresholds, num_topics);
		}
		else if (how_data_loaded == data_ingest::PREPROCESSED_DATA_LOAD) {
			offset_t chunk_size = 1 << 24;
			word_id_t *word_begins = new word_id_t[divide_round_up(offsets_CSR[vocab_size], chunk_size)];
			word_id_t *word_ends = new word_id_t[divide_round_up(offsets_CSR[vocab_size], chunk_size)];
			word_id_t word_begin = 0;
			word_id_t word_end = 0;
			word_id_t num_word_chunks = 0;
			while (word_begin < vocab_size) {
				while (offsets_CSR[word_end] - offsets_CSR[word_begin] < chunk_size && word_end < vocab_size)
					++word_end;
				word_begins[num_word_chunks] = word_begin;
				word_ends[num_word_chunks] = word_end;
				++num_word_chunks;
				word_begin = word_end;
			}
			assert(num_word_chunks <= divide_round_up(offsets_CSR[vocab_size], chunk_size));
			assert(word_ends[num_word_chunks - 1] == vocab_size);

			offset_t *new_nnzs_in_chunk = new offset_t[num_word_chunks];
			pfor(int64_t chunk = 0; chunk < num_word_chunks; ++chunk) {
				A_sp->list_word_freqs_from_CSR(word_begins[chunk], word_ends[chunk],
					normalized_vals_CSR + offsets_CSR[word_begins[chunk]],
					offsets_CSR + word_begins[chunk], freqs);
				new_nnzs_in_chunk[chunk] = A_sp->compute_thresholds(word_begins[chunk], word_ends[chunk],
					freqs, thresholds, num_topics);
				for (word_id_t word = word_begins[chunk]; word < word_ends[chunk]; ++word)
					freqs[word].clear();
			}
			new_nnzs = std::accumulate(new_nnzs_in_chunk, new_nnzs_in_chunk + num_word_chunks, (offset_t)0);
			delete[] word_begins;
			delete[] word_ends;
			delete[] new_nnzs_in_chunk;
		}
		delete[] freqs;
		assert(thresholds.size() == vocab_size);
		timer->next_time_secs("Computing thresholds");
		out_log->print_string("Number of entries above threshold: " + std::to_string(new_nnzs) + "\n");

		std::vector<doc_id_t> original_cols;
		if (flag_sample_docs) {
			assert(sample_rate > 0.0 && sample_rate < 1.0);
			B_fl_CSC->sampled_threshold_and_copy<A_TYPE>(
				*A_sp, thresholds, new_nnzs, original_cols, sample_rate);
		}
		else {
			B_fl_CSC->threshold_and_copy<A_TYPE>(
				*A_sp, thresholds, new_nnzs, original_cols);
		}
		timer->next_time_secs("Creating thresholded and scaled matrix");

		//
		// Truncated SVD with Spectra (ARPACK) or Block KS
		//
		out_log->print_string("Frob(B_fl_CSC): " + std::to_string(B_fl_CSC->frobenius()) + "\n");
		std::vector<FPTYPE> evalues;
		B_fl_CSC->initialize_for_eigensolver(num_topics);
		timer->next_time_secs("eigen solver init");
		if (EIGENSOLVER == SPECTRA)
			B_fl_CSC->compute_Spectra(num_topics, evalues);
		else if (EIGENSOLVER == BLOCK_KS)
			B_fl_CSC->compute_block_ks(num_topics, evalues);
		else
			assert(false);
		out_log->print_eigen_data(evalues, num_topics);
		auto &B_fl = B_fl_CSC;
		timer->next_time_secs("Spectra eigen solve");


		//
		// k-means++ on the column space (Simga*VT) of k-rank approx of B
		//
		if (!ENABLE_KMEANS_ON_LOWD)
			assert(KMEANS_INIT_METHOD == KMEANSPP || KMEANS_INIT_METHOD == KMEANSMCMC);

		std::vector<doc_id_t> best_kmeans_seeds;
		int num_centers_lowd = num_topics;
		FPTYPE *centers_lowd = NULL;
		FPTYPE best_residual = 0.0;
		if (ENABLE_KMEANS_ON_LOWD) centers_lowd = new FPTYPE[(size_t)num_topics * (size_t)num_centers_lowd];

		if (KMEANS_INIT_METHOD == KMEANSPP) out_log->print_string("k-means init method: KMEANSPP\n");
		if (KMEANS_INIT_METHOD == KMEANSMCMC) out_log->print_string("k-means init method: KMEANSMCMC\n");
		if (KMEANS_INIT_METHOD == KMEANSBB) out_log->print_string("k-means init method: KMEANSBB\n");

		FPDenseMatrix<FPTYPE> *B_sigmaVT_d_fl = NULL;
		if (USE_EXPLICIT_PROJECTED_MATRIX) {
			B_sigmaVT_d_fl = new FPDenseMatrix<FPTYPE>((word_id_t)num_topics, B_fl_CSC->num_docs());
			B_sigmaVT_d_fl->copy_sigmaVT_from(*B_fl, num_topics);
			best_residual = B_sigmaVT_d_fl->kmeans_init(num_centers_lowd,
				KMEANS_INIT_REPS, KMEANS_INIT_METHOD, best_kmeans_seeds, centers_lowd);
		}
		else {
			best_residual = B_fl->kmeans_init_on_projected_space(num_centers_lowd,
				KMEANS_INIT_REPS, best_kmeans_seeds, centers_lowd);
		}
		out_log->print_string("Best k-means init residual: " + std::to_string(best_residual) + "\n");
		timer->next_time_secs("K-means seeds initialization");


		//
		// Lloyds on B_k with k-means++ seeds
		//
		if (ENABLE_KMEANS_ON_LOWD) {
			if (USE_EXPLICIT_PROJECTED_MATRIX) {
				B_sigmaVT_d_fl->run_lloyds(num_centers_lowd, centers_lowd,
					NULL, MAX_KMEANS_LOWD_REPS);
				delete B_sigmaVT_d_fl;
			}
			else {
				B_fl->run_lloyds_on_projected_space(num_centers_lowd, centers_lowd,
					NULL, MAX_KMEANS_LOWD_REPS);
			}

			B_fl->left_multiply_by_U_Spectra(centers, centers_lowd, num_topics, num_topics);
			delete[] centers_lowd;
			timer->next_time_secs("Converging LLoyds k-means on B_k");
		}
		B_fl->cleanup_after_eigensolver();

		//
		// Lloyds on B with k-means++ seeds
		//
		if (!ENABLE_KMEANS_ON_LOWD)
			for (doc_id_t d = 0; d < num_topics; ++d)
				B_fl->copy_col_to(centers + (size_t)d * (size_t)vocab_size, best_kmeans_seeds[d]);
		if (KMEANS_ALGO_FOR_SPARSE == LLOYDS_KMEANS)
			B_fl->run_lloyds(num_topics, centers, closest_docs, MAX_KMEANS_REPS);
		else if (KMEANS_ALGO_FOR_SPARSE == ELKANS_KMEANS)
			B_fl->run_elkans(num_topics, centers, closest_docs, MAX_KMEANS_REPS);
		else assert(false);
		count_t closest_docs_sizes_sum = 0;
		for (int t = 0; t < num_topics; ++t)
			closest_docs_sizes_sum += closest_docs[t].size();
		assert(closest_docs_sizes_sum == B_fl->num_docs());
		timer->next_time_secs("k-means on B");

		for (doc_id_t topic = 0; topic != num_topics; ++topic)
			for (auto d = closest_docs[topic].begin(); d < closest_docs[topic].end(); ++d)
				*d = original_cols[*d];

		//
		// Identify Catchwords
		//
		MKL_UINT r;
		if (flag_sample_docs)
			r = (MKL_UINT)std::floor(eps2_c*w0_c*(FPTYPE)num_docs*sample_rate / (FPTYPE)(2.0*num_topics));
		else
			r = (MKL_UINT)std::floor(eps2_c*w0_c*(FPTYPE)num_docs / (FPTYPE)(2.0*num_topics));

		if (how_data_loaded == data_ingest::FILE_DATA_LOAD
			|| how_data_loaded == data_ingest::ITERATIVE_DATA_LOAD) {
			pfor_dynamic_16(int topic = 0; topic < num_topics; ++topic)
				A_sp->rth_highest_element(r, closest_docs[topic],
					catchword_thresholds + (size_t)topic * (size_t)vocab_size);
		}
		else if (how_data_loaded == data_ingest::PREPROCESSED_DATA_LOAD) {
			FPTYPE *threshold_matrix_tr = new FPTYPE[(size_t)num_topics * (size_t)vocab_size];

			int *cluster_ids = new int[num_docs];
			for (int64_t d = 0; d < num_docs; ++d) cluster_ids[d] = -1;
			for (int topic = 0; topic < num_topics; ++topic)
				for (auto iter = closest_docs[topic].begin(); iter < closest_docs[topic].end(); ++iter)
					cluster_ids[*iter] = topic;


			offset_t chunk_size = 1 << 18;
			word_id_t *word_begins = new word_id_t[divide_round_up(offsets_CSR[vocab_size], chunk_size)];
			word_id_t *word_ends = new word_id_t[divide_round_up(offsets_CSR[vocab_size], chunk_size)];
			word_id_t word_begin = 0;
			word_id_t word_end = 0;
			word_id_t num_word_chunks = 0;
			while (word_begin < vocab_size) {
				while (offsets_CSR[word_end] - offsets_CSR[word_begin] < chunk_size && word_end < vocab_size)
					++word_end;
				word_begins[num_word_chunks] = word_begin;
				word_ends[num_word_chunks] = word_end;
				++num_word_chunks;
				word_begin = word_end;
			}
			assert(num_word_chunks <= divide_round_up(offsets_CSR[vocab_size], chunk_size));

			pfor(int64_t chunk = 0; chunk < num_word_chunks; ++chunk) {
				A_sp->rth_highest_element_using_CSR(word_begins[chunk], word_ends[chunk],
					num_topics, r, closest_docs,
					normalized_vals_CSR + offsets_CSR[word_begins[chunk]],
					cols_CSR + offsets_CSR[word_begins[chunk]],
					offsets_CSR + word_begins[chunk],
					cluster_ids, threshold_matrix_tr);
			}
			pfor_dynamic_8192(int64_t w = 0; w < vocab_size; ++w)
				for (int t = 0; t < num_topics; ++t)
					catchword_thresholds[(size_t)w + (size_t)t * (size_t)vocab_size]
					= threshold_matrix_tr[(size_t)w * (size_t)num_topics + (size_t)t];

			delete[] word_begins;
			delete[] word_ends;
			delete[] threshold_matrix_tr;
			delete[] cluster_ids;
		}
		timer->next_time_secs("Collecting word freqs in clusters");

		A_sp->find_catchwords(num_topics, catchword_thresholds, catchwords);
		timer->next_time_secs("Finding catchwords for clusters");


		//
		// Construct the topic model
		//   
		A_sp->construct_topic_model(
			*Model, num_topics, closest_docs, catchwords,
			AVG_CLUSTER_FOR_CATCHLESS_TOPIC,
			flag_construct_edge_topics && flag_print_top_two_topics ? &top_topic_pairs : NULL,
			&catchword_topics,
			&doc_topic_sum);
		timer->next_time_secs("Constructing topic vectors");
		
		is_training_complete = true;
	}

	void ISLETrainer::write_model_to_file()
	{
		output_model(true);
		timer->next_time_secs("Output model");

		output_top_words();
		timer->next_time_secs("Output topwords");

		/* if (flag_print_doctopic) {
		output_doc_topic(catchword_topics, doc_topic_sum);
		timer->next_time_secs("Output doc-topic-catchword");
		}*/
	}

	//
	// Construct edge topics
	//
	void ISLETrainer::train_edge_topics()
	{
		if (!is_training_complete) {
			std::cerr << "Train basic topics before computing edge topics" << std::endl;
			exit(-1);
		}
		if (!flag_construct_edge_topics) {
			std::cerr << "Flag for edge topic construction must be turned on" << std::endl;
			exit(-1);
		}
		construct_edge_topics_v2(top_topic_pairs);
        timer->next_time_secs("Constructing edge topic model");
    }

    void ISLETrainer::write_edgemodel_to_file()
    {
		if (flag_construct_edge_topics) {
			output_edge_model(true);
			timer->next_time_secs("Output edge model");
		}

		//
		// Print top 2 topics for each document to file
		//
		//if (flag_construct_edge_topics && flag_print_top_two_topics)
		//	print_top_two_topics(top_topic_pairs);
		//timer->next_time_secs("Printing top 2 topics/doc");
    }

    //
    // Calculate coherence based on cluster averages, without using catchwords
    //	
    void ISLETrainer::output_avg_topic_coherence(
        FPTYPE& avg_nl_coherence,
        std::vector<FPTYPE>& nl_coherences)
    {
        assert(is_training_complete);

        DenseMatrix<FPTYPE> AvgModel(vocab_size, num_topics);
        auto null_catchwords = new std::vector<word_id_t>[num_topics];
        A_sp->construct_topic_model(
            AvgModel, num_topics, closest_docs, null_catchwords,
            AVG_CLUSTER_FOR_CATCHLESS_TOPIC);
        auto nl_top_words = new std::vector<std::pair<word_id_t, FPTYPE> >[num_topics];
        A_sp->topic_coherence(num_topics, DEFAULT_COHERENCE_NUM_WORDS, AvgModel, nl_top_words, nl_coherences);
        avg_nl_coherence = std::accumulate(nl_coherences.begin(), nl_coherences.end(), 0.0) / num_topics;
        out_log->print_string(
            "\nAvg coherence without catchwords: "
            + std::to_string(avg_nl_coherence) + "\n");
        timer->next_time_secs("computing coherence without catchwords");

        //
        // Output Model to File
        //
        AvgModel.write_to_file(concat_file_path(log_dir, std::string("M_hat_avg")));
        timer->next_time_secs("Writing Mhat to file");

        //
        // Ouput Top Words to File
        //
        std::ofstream out_top_words_avg;
        out_top_words_avg.open(concat_file_path(log_dir, std::string("TopWordsPerTopic_avg.txt")));
        for (doc_id_t topic = 0; topic < num_topics; ++topic) {
            for (auto iter = nl_top_words[topic].begin(); iter < nl_top_words[topic].end(); ++iter)
                out_top_words_avg << vocab_words[iter->first] << "\t";
            out_top_words_avg << std::endl;
        }
        out_top_words_avg.close();
        timer->next_time_secs("Writing top words to file");

        delete[] nl_top_words;
    }


    //
    // Calculate Topic Diversity
    //
    void ISLETrainer::output_topic_diversity()
    {
        assert(is_training_complete);

        auto avg_topic_vector = new FPTYPE[A_sp->vocab_size()];
        for (word_id_t w = 0; w < Model->vocab_size(); ++w) avg_topic_vector[w] = 0.0;
        for (auto t = 0; t < num_topics; ++t)
            FPaxpy(Model->vocab_size(), 1.0 / num_topics, Model->data() + Model->vocab_size() * (size_t)t, 1,
                avg_topic_vector, 1);
        auto l2sq_dist_to_avg_topic = new FPTYPE[num_topics];
        FPTYPE l2sq_avg_topic_vector = FPdot(Model->vocab_size(), avg_topic_vector, 1, avg_topic_vector, 1);
        for (auto t = 0; t < num_topics; ++t) {
            l2sq_dist_to_avg_topic[t] = l2sq_avg_topic_vector
                + FPdot(Model->vocab_size(), Model->data() + Model->vocab_size() * (size_t)t, 1,
                    Model->data() + Model->vocab_size() * (size_t)t, 1)
                - 2.0 * FPdot(Model->vocab_size(), Model->data() + Model->vocab_size(), 1,
                    avg_topic_vector, 1);
        }
        auto avg_diversity = std::accumulate(l2sq_dist_to_avg_topic, l2sq_dist_to_avg_topic + num_topics, (FPTYPE)0.0) / num_topics;
        out_log->print_string("\n Average topic diversity: " + std::to_string(avg_diversity) + "\n\n");
        timer->next_time_secs("Calculating diversity");
    }

    //
    // Output Catchwords and Dominant Words for each topic
    //
    void ISLETrainer::output_cluster_summary()
    {
        assert(is_training_complete);

		for (auto topic = 0; topic < num_topics; ++topic)
			Model->find_n_top_words(topic,
				DEFAULT_COHERENCE_NUM_WORDS > 10? DEFAULT_COHERENCE_NUM_WORDS:10,
				topwords[topic]);

        Timer timer;
        FPTYPE avg_nl_coherence;
        std::vector<FPTYPE> nl_coherences(num_topics, 0.0);
        /*if (flag_compute_avg_coherence)
            output_avg_topic_coherence(avg_nl_coherence, nl_coherences);
        timer.next_time_secs("Calculating average coherence");*/


        std::vector<FPTYPE> coherences(num_topics, 0.0);
        //A_sp->topic_coherence(num_topics, DEFAULT_COHERENCE_NUM_WORDS, *Model, topwords, coherences);
	    FPTYPE avg_coherence =std::accumulate(coherences.begin(), coherences.end(), 0.0)  / coherences.size();
        timer.next_time_secs("Calculating coherence");

        for (doc_id_t t = 0; t < num_topics; ++t) {
            out_log->print_string("\n---------- Topic: " + std::to_string(t)
                + ", Cluster_size: " + std::to_string(closest_docs[t].size()) + " -----------\n", false);
            out_log->print_catch_words<A_TYPE>(t, catchword_thresholds + (size_t)t * A_sp->vocab_size(),
                catchwords[t], vocab_words, false);
            std::ostringstream top_word_stream;
            Model->print_top_words(vocab_words, topwords[t], top_word_stream);
            out_log->print_stringstream(top_word_stream, false);
            //out_log->print_string("\nCoherence: " + std::to_string(coherences[t]) + "\n", false);
        }
        out_log->print_string("\n---------------------------\n", false);
        if (flag_compute_avg_coherence)
            out_log->print_string("\n Avg coherence without catchwords : " + std::to_string(avg_nl_coherence) + "\n");
        out_log->print_string("\n Avg coherence: " + std::to_string(avg_coherence) + "\n\n");


        //
        // Print Cluster Details
        //
        std::vector<FPTYPE> distsq(num_topics, 0.0);
        //for (auto i = 0; i < num_topics; ++i) {
            //   for (auto diter = closest_docs[i].begin(); diter != closest_docs[i].end(); ++diter)
            //       distsq[i] += A_sp->distsq_normalized_doc_to_pt(*diter, centers + (size_t)i * (size_t)vocab_size);
        //}
        out_log->print_cluster_details(num_topics, distsq, catchwords, closest_docs, coherences, nl_coherences);
        timer.next_time_secs("Output summary");
        output_top_words();

    }

    //
    // Output Model to file
    //
    void ISLETrainer::output_model(bool output_sparse)
    {
        assert(is_training_complete);

        //Model->write_to_file(concat_file_path(log_dir, std::string("M_hat_catch")));
        if (output_sparse)
            Model->write_to_file_as_sparse(concat_file_path(log_dir, std::string("M_hat_catch_sparse")));
    }

    //
    // Output EdgeModel to file
    //
    void ISLETrainer::output_edge_model(bool output_sparse)
    {
        assert(is_training_complete);

        //EdgeModel->write_to_file(concat_file_path(log_dir, std::string("EdgeModel")));
        if (output_sparse)
            EdgeModel->write_to_file_as_sparse(concat_file_path(log_dir, std::string("EdgeModel_sparse")));
    }

    //
    // Ouput Top Words to File
    //
    void ISLETrainer::output_top_words()
    {
        assert(is_training_complete);

        std::ofstream out_top_words;
        out_top_words.open(concat_file_path(log_dir, std::string("TopWordsPerTopic_catch.txt")));
        for (doc_id_t topic = 0; topic < num_topics; ++topic) {
            for (auto iter = topwords[topic].begin(); iter < topwords[topic].end(); ++iter)
                out_top_words << vocab_words[iter->first] << "\t";
            out_top_words << std::endl;
        }
        out_top_words.close();
        timer->next_time_secs("Writing top words to file");
    }

    //
    // Output document catchword frequencies in 1-based index
    // Output doc-topic catchword sums in 1-based index
    //
    void ISLETrainer::output_doc_topic(
        std::vector<std::pair<word_id_t, int> >& catchword_topics,
        std::vector<std::tuple<doc_id_t, doc_id_t, FPTYPE> >& doc_topic_sum)
    {
        assert(is_training_complete);

        /*std::vector<std::pair<word_id_t, doc_id_t> > catch_topic;
        for (auto topic = 0; topic < num_topics; ++topic)
            for (auto iter = catchwords[topic].begin(); iter < catchwords[topic].end(); ++iter)
            catch_topic.push_back(std::make_pair(*iter, topic));*/
        parallel_sort(catchword_topics.begin(), catchword_topics.end(),
            [](const auto& l, const auto& r) {return l.first < r.first; });
        out_log->print_string("Total number of catchwords: " + std::to_string(catchword_topics.size()) + "\n");

        //Matrix Too big!!
        //DenseMatrix<FPTYPE> DocTopic(num_topics, num_docs);
        std::string filename = concat_file_path(log_dir, std::string("DocCatchword.tsv"));

#if FILE_IO_MODE == NAIVE_FILE_IO
        { // Fill in code here

            Timer DTtimer;
            std::ofstream doc_catch_out;
            doc_catch_out.open(filename);
            for (doc_id_t doc = 0; doc < A_sp->num_docs(); ++doc) {
                auto w_iter = catchword_topics.begin();
                for (offset_t pos = A_sp->offset_CSC(doc); pos < A_sp->offset_CSC(doc + 1); ++pos) {
                    while (w_iter != catchword_topics.end() && w_iter->first < A_sp->row_CSC(pos))
                        ++w_iter;
                    if (w_iter == catchword_topics.end())
                        continue;
                    if (A_sp->row_CSC(pos) == w_iter->first) {
                        //assert(A_sp->normalized(w_iter->first, doc) == A_sp->normalized_val_CSC(pos));
                        //assert(A_sp->normalized_val_CSC(pos) > 0.0);

                        //DocTopic.elem_ref(w_iter->second, doc) += A_sp->normalized_val_CSC(pos);

                        doc_catch_out << doc + 1 << '\t';
                        doc_catch_out << (w_iter->first) + 1 << '\t';
                        doc_catch_out << A_sp->normalized_val_CSC(pos) << '\n';
                    }
                }
            }
            DTtimer.next_time_secs("DT: doc-catch", 30);
            doc_catch_out.close();
            DTtimer.next_time_secs("DT: flush,close", 30);

            std::string filename = concat_file_path(log_dir, std::string("DocTopicCatchwordSums.tsv"));
            std::ofstream doc_topic_out;
            doc_topic_out.open(filename);
            /*for (word_id_t doc = 0; doc < A_sp->num_docs(); ++doc)
                for (auto topic = 0; topic < num_topics; ++topic) {
                    if (DocTopic.elem(topic, doc) > 0.0) {
                        doc_topic_out << doc + 1 <<  '\t';
                        doc_topic_out << topic + 1 << '\t';
                        doc_topic_out << DocTopic.elem(topic, doc) << '\n';
                    }
                    }*/
            for (auto iter = doc_topic_sum.begin(); iter < doc_topic_sum.end(); ++iter)
                doc_topic_out << std::get<0>(*iter) + 1 << "\t"
                << std::get<1>(*iter) + 1 << "\t"
                << std::get<2>(*iter) << "\n";
            DTtimer.next_time_secs("DT: doc-topic", 30);
            doc_topic_out.close();
            DTtimer.next_time_secs("DT: flush,close", 30);
        }
        else;

#elif FILE_IO_MODE == WIN_MMAP_FILE_IO || FILE_IO_MODE == LINUX_MMAP_FILE_IO
        {
            Timer DTtimer;
            MMappedOutput out(filename);
            for (doc_id_t doc = 0; doc < A_sp->num_docs(); ++doc) {
                auto w_iter = catchword_topics.begin();
                for (offset_t pos = A_sp->offset_CSC(doc); pos < A_sp->offset_CSC(doc + 1); ++pos) {
                    while (w_iter != catchword_topics.end() && w_iter->first < A_sp->row_CSC(pos))
                        ++w_iter;
                    if (w_iter == catchword_topics.end())
                        continue;
                    if (A_sp->row_CSC(pos) == w_iter->first) {
                        //assert(A_sp->normalized(w_iter->first, doc) == A_sp->normalized_val_CSC(pos));
                        //assert(A_sp->normalized_val_CSC(pos) > 0.0);

                        // DocTopic.elem_ref(w_iter->second, doc) += A_sp->normalized_val_CSC(pos);

                        out.concat_int(doc + 1, '\t');
                        out.concat_int((w_iter->first) + 1, '\t');
                        out.concat_float(A_sp->normalized_val_CSC(pos), '\n');
                    }
                }
            }
            DTtimer.next_time_secs("DT: doc-catch", 30);
            out.flush_and_close();
            DTtimer.next_time_secs("DT: flush,close", 30);

            std::string filename = concat_file_path(log_dir, std::string("DocTopicCatchwordSums.tsv"));
            MMappedOutput out_doc_topic(filename);
            /*for (word_id_t doc = 0; doc < A_sp->num_docs(); ++doc)
                for (auto topic = 0; topic < num_topics; ++topic) {
                    if (DocTopic.elem(topic, doc) > 0.0) {
                        out_doc_topic.concat_int(doc + 1, '\t');
                        out_doc_topic.concat_int(topic + 1, '\t');
                        out_doc_topic.concat_float(DocTopic.elem(topic, doc), '\n');
                    }
                    }*/
            for (auto iter = doc_topic_sum.begin(); iter < doc_topic_sum.end(); ++iter) {
                out_doc_topic.concat_int(std::get<0>(*iter) + 1, '\t');
                out_doc_topic.concat_int(std::get<1>(*iter) + 1, '\t');
                out_doc_topic.concat_float(std::get<2>(*iter), '\n');
            }

            DTtimer.next_time_secs("DT: doc-topic", 30);
            out_doc_topic.flush_and_close();
            DTtimer.next_time_secs("DT: flush,close", 30);
        }
#endif
        timer->next_time_secs("Writing document catchword weights");
    }

    void ISLETrainer::get_basic_model(FPTYPE* basicModel)
    {
        memcpy(basicModel, Model->data(), (size_t)vocab_size * (size_t)num_topics * sizeof(FPTYPE));
    }

    int ISLETrainer::get_num_edge_topics()
    {
        return EdgeModel->num_docs();
    }

    void ISLETrainer::get_edge_model(FPTYPE* edge_model)
    {
        memcpy(edge_model, EdgeModel->data(), (size_t)vocab_size * (size_t)EdgeModel->num_docs() * sizeof(FPTYPE));
    }

    void ISLETrainer::print_top_two_topics(
        std::vector<std::tuple<int, int, doc_id_t> >& top_topic_pairs)
    {
        parallel_sort(top_topic_pairs.begin(), top_topic_pairs.end(),
            [](const auto& l, const auto& r)
        { return std::get<2>(l) < std::get<2>(r); });

        std::string filename = concat_file_path(log_dir, std::string("TopTwoTopicsPerDoc.txt"));
#if FILE_IO_MODE == NAIVE_FILE_IO

        std::ofstream toptwotopics_out;
        toptwotopics_out.open(filename);
        for (auto iter = top_topic_pairs.begin(); iter != top_topic_pairs.end(); ++iter) {
            toptwotopics_out << std::get<2>(*iter) << '\t'
                << std::get<0>(*iter) << '\t'
                << std::get<1>(*iter) << '\t';
        }
        toptwotopics_out.close();

#elif FILE_IO_MODE == WIN_MMAP_FILE_IO || FILE_IO_MODE == LINUX_MMAP_FILE_IO

        MMappedOutput out(filename);
        for (auto iter = top_topic_pairs.begin(); iter != top_topic_pairs.end(); ++iter) {
            out.concat_int(std::get<2>(*iter) + 1, '\t');
            out.concat_int(std::get<0>(*iter) + 1, '\t');
            out.concat_int(std::get<1>(*iter) + 1, '\n');
        }
        out.flush_and_close();

#else
        assert(false);
#endif
    }

    void ISLETrainer::construct_edge_topics_v1(
        std::vector<std::tuple<int, int, doc_id_t> >& top_topic_pairs,
        bool flag_print_edge_topic_composition)
    {
        parallel_sort(top_topic_pairs.begin(), top_topic_pairs.end(),
            [](const auto&l, const auto&r) {return std::get<0>(l) < std::get<0>(r)
            || (std::get<0>(l) == std::get<0>(r) && std::get<1>(l) < std::get<1>(r)); });
        std::cout << "#Top topic pairs for compound topics: " << top_topic_pairs.size() << std::endl;

        std::vector<std::tuple<int, int, count_t> > sorted_ctopics;
        for (auto iter = top_topic_pairs.begin(); iter != top_topic_pairs.end(); ) {
            auto next_iter = std::upper_bound(iter, top_topic_pairs.end(), *iter,
                [](const auto&l, const auto&r) {return std::get<0>(l) < std::get<0>(r)
                || (std::get<0>(l) == std::get<0>(r) && std::get<1>(l) < std::get<1>(r)); });
            if (next_iter - iter >= EDGE_TOPIC_MIN_DOCS)
                sorted_ctopics.push_back(std::make_tuple(std::get<0>(*iter), std::get<1>(*iter), next_iter - iter));
            iter = next_iter;
        }

        parallel_sort(sorted_ctopics.begin(), sorted_ctopics.end(),
            [](const auto& l, const auto &r) {return std::get<2>(l) > std::get<2>(r); });
        int running_count = 0; int edge_topic_threshold;
        for (auto iter = sorted_ctopics.begin(); iter != sorted_ctopics.end(); ++iter) {
            if (++running_count > max_edge_topics) {
                edge_topic_threshold = std::get<2>(*iter);
                break;
            }
        }

        std::vector<std::tuple<int, int, count_t> > selected_pairs;
        for (auto iter = top_topic_pairs.begin();
            iter != top_topic_pairs.end() && selected_pairs.size() <= max_edge_topics;) {
            auto next_iter = std::upper_bound(iter, top_topic_pairs.end(), *iter,
                [](const auto&l, const auto&r) {return std::get<0>(l) < std::get<0>(r)
                || (std::get<0>(l) == std::get<0>(r) && std::get<1>(l) < std::get<1>(r)); });
            if (next_iter - iter >= EDGE_TOPIC_MIN_DOCS && next_iter - iter >= edge_topic_threshold)
                selected_pairs.push_back(std::make_tuple(std::get<0>(*iter), std::get<1>(*iter), next_iter - iter));
            iter = next_iter;
        }

	count_t docs_selected = 0;
	for(const auto &sel_pair : selected_pairs){
		docs_selected += std::get<2>(sel_pair);
	}
	std::cout << "#Edge topics: " << selected_pairs.size() << std::endl
            << "#Docs selected for edge topics: "
            << docs_selected << std::endl
            << "Doc Count threshold for edge topics: " << edge_topic_threshold << std::endl;

	/*
        std::cout << "#Edge topics: " << selected_pairs.size() << std::endl
            << "#Docs selected for edge topics: "
            << std::accumulate(selected_pairs.begin(), selected_pairs.end(), 0,
                [](const auto& lval, const auto& iter) {return lval + std::get<2>(iter); }) << std::endl
            << "Doc Count threshold for edge topics: " << edge_topic_threshold << std::endl;
*/
        EdgeModel = new DenseMatrix<FPTYPE>(vocab_size, selected_pairs.size());
        pfor_dynamic_16(int ctopic = 0; ctopic < selected_pairs.size(); ++ctopic) {
            auto range = std::equal_range(top_topic_pairs.begin(), top_topic_pairs.end(),
                std::make_tuple(std::get<0>(selected_pairs[ctopic]), std::get<1>(selected_pairs[ctopic]), 0),
                [](const auto& l, const auto&r) { return std::get<0>(l) < std::get<0>(r)
                || (std::get<0>(l) == std::get<0>(r) && std::get<1>(l) < std::get<1>(r)); });
            auto nDocs = std::get<2>(selected_pairs[ctopic]);
            for (auto iter = range.first; iter != range.second; ++iter) {
                auto doc = std::get<2>(*iter);
                for (auto pos = A_sp->offset_CSC(doc); pos < A_sp->offset_CSC(doc + 1); ++pos)
                    EdgeModel->elem_ref(A_sp->row_CSC(pos), ctopic) += (FPTYPE)A_sp->normalized_val_CSC(pos) / nDocs;
            }
        }

        if (flag_print_edge_topic_composition)
            print_edge_topic_composition(selected_pairs);
    }

    void ISLETrainer::construct_edge_topics_v2(
        std::vector<std::tuple<int, int, doc_id_t> >& top_topic_pairs,
        bool flag_print_edge_topic_composition)
    {
        parallel_sort(top_topic_pairs.begin(), top_topic_pairs.end(),
            [](const auto&l, const auto&r) {return std::get<0>(l) < std::get<0>(r)
            || (std::get<0>(l) == std::get<0>(r) && std::get<1>(l) < std::get<1>(r)); });

        std::vector<std::tuple<int, int, count_t> > selected_pairs;
        for (auto iter = top_topic_pairs.begin(); iter != top_topic_pairs.end(); ) {
            auto next_iter = std::upper_bound(iter, top_topic_pairs.end(), *iter,
                [](const auto&l, const auto&r) {return std::get<0>(l) < std::get<0>(r)
                || (std::get<0>(l) == std::get<0>(r) && std::get<1>(l) < std::get<1>(r)); });
            if (next_iter - iter >= EDGE_TOPIC_MIN_DOCS)
                selected_pairs.push_back(std::make_tuple(std::get<0>(*iter), std::get<1>(*iter), next_iter - iter));
            iter = next_iter;
        }
        std::cout << "#Candidates for edge topics: " << selected_pairs.size() << std::endl;

        parallel_sort(selected_pairs.begin(), selected_pairs.end(),
            [](const auto& l, const auto &r) {return std::get<2>(l) > std::get<2>(r); });
        int running_count = 0;
        int edge_topic_threshold;
        for (auto iter = selected_pairs.begin(); iter != selected_pairs.end(); ++iter) {
            if (++running_count > max_edge_topics) {
                edge_topic_threshold = std::get<2>(*iter); 
                selected_pairs.erase(iter, selected_pairs.end());
                break;
            }
        }

        std::cout << "Edge topic threshold: " << edge_topic_threshold << std::endl;
        std::cout << "#Edge topics: " << selected_pairs.size() << std::endl;

        EdgeModel = new DenseMatrix<FPTYPE>(vocab_size, selected_pairs.size());
        assert(Model != NULL);

        pfor_dynamic_8192 (int64_t t = 0; t < selected_pairs.size(); ++t) {
            FPaxpy(vocab_size, EDGE_TOPIC_PRIMARY_RATIO,
                Model->data() + std::get<0>(selected_pairs[t]) * vocab_size, 1,
                EdgeModel->data() + t * vocab_size, 1);
            FPaxpy(vocab_size, 1 - EDGE_TOPIC_PRIMARY_RATIO,
                Model->data() + std::get<1>(selected_pairs[t]) * vocab_size, 1,
                EdgeModel->data() + t * vocab_size, 1);
        }
        std::cout << "Completed edge topic construction" << std::endl;

        if (flag_print_edge_topic_composition) {
            print_edge_topic_composition(selected_pairs);
            print_edge_topic_top_words(selected_pairs, 10);
        }
    }

    void ISLETrainer::print_edge_topic_composition(
        std::vector<std::tuple<int, int, count_t> >& topic_pairs)
    {
        std::string filename = concat_file_path(log_dir, std::string("EdgeTopicComposition.txt"));

#if FILE_IO_MODE == NAIVE_FILE_IO
        std::ofstream out;
        out.open(filename);
        for (auto iter = topic_pairs.begin(); iter != topic_pairs.end(); ++iter) {
            out << std::get<0>(*iter) << '\t'
                << std::get<1>(*iter) << '\t'
                << std::get<2>(*iter) << '\n';
        }
        out.close();
#elif FILE_IO_MODE == WIN_MMAP_FILE_IO || FILE_IO_MODE == LINUX_MMAP_FILE_IO
        MMappedOutput out(filename);
        for (auto iter = topic_pairs.begin(); iter != topic_pairs.end(); ++iter) {
            out.concat_int(std::get<0>(*iter), '\t');
            out.concat_int(std::get<1>(*iter), '\t');
            out.concat_int(std::get<2>(*iter), '\n');
        }
        out.flush_and_close();
#else
        assert(false);
#endif
    }

    void ISLETrainer::print_edge_topic_top_words(
        std::vector<std::tuple<int, int, count_t> >& topic_pairs,
        const int num_top_words)
    {
        assert(EdgeModel != NULL);

        std::string filename = concat_file_path(log_dir, std::string("EdgeTopicTopWords.txt"));
        std::ofstream out;
        out.open(filename);


        for (auto iter = topic_pairs.begin(); iter != topic_pairs.end(); ++iter) {
            auto t = iter - topic_pairs.begin();
            out << "Edge Topic: " << t
                << "  (" << std::get<0>(*iter) << ", "
                << std::get<1>(*iter) << "): "
                << std::get<2>(*iter) << '\n';

            /*std::vector<std::pair<word_id_t, FPTYPE> > weights;
            for (word_id_t w = 0; w < EdgeModel->vocab_size(); ++w)
                weights.push_back(std::make_pair(w, EdgeModel->elem(w, t)));
            std::sort(weights.begin(), weights.end(),
                [](const auto& l, const auto& r) {return l.second >= r.second; });*/

            std::vector<std::pair<word_id_t, FPTYPE> > top_words;
            EdgeModel->find_n_top_words(t, 2* num_top_words, top_words);
            out << "Top words in edge_topic: \n";
            for (int word = 0; word < 2 * num_top_words; ++word)
                out << vocab_words[top_words[word].first]
                << "(" << top_words[word].first << "," << top_words[word].second << ")\t";
            out << "\n";


            Model->find_n_top_words(std::get<0>(*iter), num_top_words, top_words);
            out << "Top words in topic: " << std::get<0>(*iter) << "\n";
            for (int word = 0; word < num_top_words; ++word)
                out << vocab_words[top_words[word].first]
                << "(" << top_words[word].first << "," << top_words[word].second << ")\t";
            out << "\n";


            Model->find_n_top_words(std::get<1>(*iter), num_top_words, top_words);
            out << "Top words in topic: " << std::get<1>(*iter) << "\n";
            for (int word = 0; word < num_top_words; ++word)
                out << vocab_words[top_words[word].first]
                << "(" << top_words[word].first << "," << top_words[word].second << ")\t";
            out << "\n\n";
        }
        out.close();
    }
}
