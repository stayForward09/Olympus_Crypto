#include "validation.hpp"
#include <mcp/core/genesis.hpp>
#include <mcp/common/stopwatch.hpp>

mcp::validation::validation(
	mcp::block_store& store_a, mcp::ledger& ledger_a,
	mcp::mru_list<mcp::block_hash>& invalid_block_cache_a,
	std::shared_ptr<mcp::block_cache> cache_a
) :
	m_store(store_a),
	m_ledger(ledger_a),
	m_graph(store_a),
	m_invalid_block_cache(invalid_block_cache_a),
	m_cache(cache_a)
{
}

mcp::validation::~validation()
{
}

mcp::base_validate_result mcp::validation::base_validate(mcp::db::db_transaction& transaction_a, std::shared_ptr<mcp::iblock_cache> cache_a, std::shared_ptr<mcp::block_processor_item> item_a)
{
	joint_message message(item_a->joint);
	assert_x(message.block);

	std::shared_ptr<mcp::block> block(message.block);
	auto block_hash(block->hash());
	assert_x(block_hash != mcp::genesis::block_hash);

	mcp::base_validate_result result;

	//check if block is in invalid block cache;
	if (!item_a->is_local() && m_invalid_block_cache.contains(block_hash))
	{
		result.code = mcp::base_validate_result_codes::known_invalid_block;
		return result;
	}

	auto exists(cache_a->block_exists(transaction_a, block_hash));
	if (exists)
	{
		result.code = mcp::base_validate_result_codes::old;
		result.err_msg = "old dag block";
		return result;
	}

	//check parents
	mcp::block_hash const & previous(block->previous());
	std::vector<mcp::block_hash> const & parents(block->parents());
	bool previous_in_parents = previous.is_zero();
	mcp::block_hash pre_pblock_hash(0);
	for (mcp::block_hash const & pblock_hash : parents)
	{
		//check if parents contains previous 
		bool is_previous(pblock_hash == previous);
		if (!previous_in_parents && is_previous)
			previous_in_parents = true;

		//check order
		if (pblock_hash <= pre_pblock_hash)
		{
			result.code = mcp::base_validate_result_codes::invalid_block;
			result.err_msg = "parent hash not ordered";
			return result;
		}
		pre_pblock_hash = pblock_hash;
	}

	if (!previous_in_parents)
	{
		result.code = mcp::base_validate_result_codes::invalid_block;
		result.err_msg = boost::str(boost::format("previous %1% not included by parents") % previous.to_string());
		return result;
	}

	////check links
	//auto links(block->links());
	//std::unordered_set<dev::Address> link_accounts;
	//mcp::block_hash pre_link_hash(0);
	//for (auto it(links.begin()); it != links.end(); it++)
	//{
	//	h256 const & link(*it);
	//	//check order
	//	if (link <= pre_link_hash)
	//	{
	//		result.code = mcp::base_validate_result_codes::invalid_block;
	//		result.err_msg = "link hash not ordered";
	//		return result;
	//	}
	//	pre_link_hash = link;
	//}

	//validate signature
	dev::Public pubkey = dev::recover(block->signature(), block_hash.number());
	if (dev::toAddress(pubkey) != block->from())   //todo used eth sign----------------------------------------
	{
		result.code = mcp::base_validate_result_codes::invalid_signature;
		return result;
	}

	result.code = mcp::base_validate_result_codes::ok;
	return result;
}

mcp::validate_result mcp::validation::dag_validate(mcp::db::db_transaction & transaction_a, std::shared_ptr<mcp::iblock_cache> cache_a, mcp::joint_message const &message)
{
	mcp::validate_result result;

	std::shared_ptr<mcp::block> block(message.block);
	auto block_hash(block->hash());

	auto exists(cache_a->block_exists(transaction_a, block_hash));
	if (exists)
	{
		result.code = mcp::validate_result_codes::old;
		return result;
	}

	//check previous
	mcp::block_hash const & previous(block->previous());
	if (!previous.is_zero())
	{
		std::shared_ptr<mcp::block> previous_block = cache_a->block_get(transaction_a, previous);
		if (!previous_block)
		{
			if (m_invalid_block_cache.contains(previous))
			{
				result.code = mcp::validate_result_codes::parents_and_previous_include_invalid_block;
				result.err_msg = boost::str(boost::format("Invalid missing previous, hash: %1%") % previous.to_string());
				return result;
			}

			result.missing_parents_and_previous.insert(previous);
		}
		else
		{
			//check previous from
			if (previous_block->from() != block->from())
			{
				result.code = mcp::validate_result_codes::invalid_block;
				result.err_msg = boost::str(boost::format("block from %1% not equal to previous from %2%") % block->from().hexPrefixed() % previous_block->from().hexPrefixed());
				return result;
			}

			//check previous exe_timestamp
			if (previous_block->exec_timestamp() > block->exec_timestamp())
			{
				result.code = mcp::validate_result_codes::invalid_block;
				result.err_msg = boost::str(boost::format("Invalid exec_timestamp, previous: %1%") % previous.to_string());
				return result;
			}
		}
	}

	//check parents
	std::vector<mcp::block_hash> const & parents(block->parents());
	for (mcp::block_hash const & pblock_hash : parents)
	{
		std::shared_ptr<mcp::block> pblock(cache_a->block_get(transaction_a, pblock_hash));
		if (!pblock)
		{
			if (m_invalid_block_cache.contains(previous))
			{
				result.code = mcp::validate_result_codes::parents_and_previous_include_invalid_block;
				result.err_msg = boost::str(boost::format("Invalid missing parent, hash: %1%") % pblock_hash.to_string());
				return result;
			}

			result.missing_parents_and_previous.insert(pblock_hash);
			continue;
		}

		//if (pblock->type() != mcp::block_type::dag && pblock->type() != mcp::block_type::genesis)
		//{
		//	result.code = mcp::validate_result_codes::invalid_block;
		//	result.err_msg = boost::str(boost::format("parent %1% is not dag block") % pblock_hash.to_string());
		//	return result;
		//}

		//check exec_timestamp
		if (pblock->exec_timestamp() > block->exec_timestamp())
		{
			result.code = mcp::validate_result_codes::invalid_block;
			result.err_msg = boost::str(boost::format("Invalid exec_timestamp, parent: %1%") % pblock_hash.to_string());
			return result;
		}
	}

	if (result.missing_parents_and_previous.size() > 0 || result.missing_links.size() > 0)
	{
		result.code = mcp::validate_result_codes::missing_parents_and_previous;
		return result;
	}

	std::shared_ptr<mcp::block_state> last_summary_block_state(cache_a->block_state_get(transaction_a, block->last_summary_block()));
	if (!last_summary_block_state
		|| !last_summary_block_state->is_stable
		|| !last_summary_block_state->is_on_main_chain
		|| !last_summary_block_state->main_chain_index)
	{
		result.code = mcp::validate_result_codes::invalid_block;
		result.err_msg = boost::str(boost::format("invalid last summary block %1%") % block->last_summary_block().to_string());
		return result;
	}

	uint64_t const & last_summary_mci(*last_summary_block_state->main_chain_index);

	//check from account is witness
	if (!mcp::param::is_witness(last_summary_mci, block->from()))
	{
		result.code = mcp::validate_result_codes::invalid_block;
		result.err_msg = boost::str(boost::format("account %1% is not witness") % block->from().hexPrefixed());
		return result;
	}

	mcp::block_param const & b_param(mcp::param::block_param(last_summary_mci));

	size_t const & parent_size(parents.size());
	if (parent_size == 0 || parent_size > b_param.max_parent_size)
	{
		result.code = mcp::validate_result_codes::invalid_block;
		result.err_msg = boost::str(boost::format("invalid parents size: %1%") % parent_size);
		return result;
	}
	
	//size_t const & link_size(links->size());
	//if (link_size > b_param.max_link_size)
	//{
	//	result.code = mcp::validate_result_codes::invalid_block;
	//	result.err_msg = boost::str(boost::format("invalid links size: %1%") % link_size);
	//	return result;
	//}

	//check parent related
	std::list<mcp::block_hash> pre_pblock_hashs;
	for (mcp::block_hash const & pblock_hash : parents)
	{
		if (pblock_hash == previous)
			continue;

		for (mcp::block_hash const &pre_hash : pre_pblock_hashs)
		{
			mcp::graph_compare_result graph_result(m_graph.compare(transaction_a, cache_a, pblock_hash, pre_hash));
			if (graph_result != mcp::graph_compare_result::non_related)
			{
				result.code = mcp::validate_result_codes::invalid_block;
				result.err_msg = boost::str(boost::format("parent %1% is related to parent %2%") % pblock_hash.to_string() % pre_hash.to_string());
				return result;
			}
		}
		pre_pblock_hashs.push_back(pblock_hash);
	}

	//check best parent
	mcp::block_hash best_pblock_hash(m_ledger.determine_best_parent(transaction_a, cache_a, parents));
	if (best_pblock_hash.is_zero())
	{
		result.code = mcp::validate_result_codes::invalid_block;
		result.err_msg = "no compatible best parent";
		return result;
	}

	mcp::witness_param const & w_param(mcp::param::witness_param(last_summary_mci));

	//check majority different of witnesses
	bool is_diff_majority(m_ledger.check_majority_witness(transaction_a, cache_a, best_pblock_hash, block->from(), w_param));
	if (!is_diff_majority)
	{
		result.code = mcp::validate_result_codes::invalid_block;
		result.err_msg = "check majority different of witnesses failed";
		return result;
	}

	//last summary
	{
		//mcp::stopwatch_guard sw("validate:last summary");

		std::shared_ptr<mcp::block> best_pblock(cache_a->block_get(transaction_a, best_pblock_hash));
		assert_x(best_pblock);

		mcp::block_hash bp_last_stable_block_hash = best_pblock->last_stable_block();
		if (best_pblock_hash == mcp::genesis::block_hash)
			bp_last_stable_block_hash = mcp::genesis::block_hash;

		if (block->last_summary_block() != bp_last_stable_block_hash)
		{
			result.code = mcp::validate_result_codes::invalid_block;
			result.err_msg = boost::str(boost::format("last summary block %1% is not equal to last stable block of best parent %2%")
				% block->last_summary_block().to_string() % bp_last_stable_block_hash.to_string());
			return result;
		}

		mcp::summary_hash last_summary;
		bool last_summary_exists(!m_cache->block_summary_get(transaction_a, block->last_summary_block(), last_summary));
		assert_x_msg(last_summary_exists, boost::str(boost::format("last summary not found, last_summary_block: %1%") % block->last_summary_block().to_string()));

		if (last_summary != block->last_summary())
		{
			result.code = mcp::validate_result_codes::invalid_block;
			result.err_msg = boost::str(boost::format("last summary %1% and last summary block %2% do not match") % block->last_summary().to_string() % block->last_summary_block().to_string());
			return result;
		}
	}

	//check last stable block
	mcp::block_hash const & last_stable_block_hash(block->last_stable_block());
	std::shared_ptr<mcp::block_state> last_stable_block_state(cache_a->block_state_get(transaction_a, last_stable_block_hash));
	if (!last_stable_block_state)
	{
		result.code = mcp::validate_result_codes::invalid_block;
		result.err_msg = boost::str(boost::format("last stable block %1% not exists") % block->last_stable_block().to_string());
		return result;
	}

	if (!last_stable_block_state->is_on_main_chain)
	{
		result.code = mcp::validate_result_codes::invalid_block;
		result.err_msg = boost::str(boost::format("last stable block %1% is not on main chain") % block->last_stable_block().to_string());
		return result;
	}

	assert_x(last_stable_block_state->main_chain_index);
	uint64_t last_stable_block_mci(*last_stable_block_state->main_chain_index);

	{
		mcp::block_hash max_p_last_stable_block_hash = mcp::genesis::block_hash;
		//mcp::stopwatch_guard sw("validate:last stable block stable1");
		uint64_t max_p_last_stable_block_mci(0);
		for (mcp::block_hash const & pblock_hash : parents)
		{
			if (pblock_hash == mcp::genesis::block_hash)
				continue;

			std::shared_ptr<mcp::block> pblock(cache_a->block_get(transaction_a, pblock_hash));
			assert_x(pblock);

			std::shared_ptr<mcp::block_state> p_last_stable_block_state(cache_a->block_state_get(transaction_a, pblock->last_stable_block()));
			assert_x(p_last_stable_block_state);
			assert_x(p_last_stable_block_state->main_chain_index);

			if (max_p_last_stable_block_mci < *p_last_stable_block_state->main_chain_index)
			{
				max_p_last_stable_block_mci = *p_last_stable_block_state->main_chain_index;
				max_p_last_stable_block_hash = pblock->last_stable_block();
			}
		}

		//check last stable mci retreat
		std::shared_ptr<mcp::block_state> max_p_last_stable_block_state(cache_a->block_state_get(transaction_a, max_p_last_stable_block_hash));
		assert_x(max_p_last_stable_block_state);
		assert_x(max_p_last_stable_block_state->main_chain_index);

		if (last_stable_block_mci < max_p_last_stable_block_mci)
		{
			result.code = mcp::validate_result_codes::invalid_block;
			result.err_msg = boost::str(boost::format("last stable block mci %1% retreat, max parent last stable block mci %2%") % last_stable_block_mci % max_p_last_stable_block_mci);
			return result;
		}

		if (last_stable_block_mci > max_p_last_stable_block_mci)
		{
			//mcp::stopwatch_guard sw("validate:last stable block stable1_1");

			bool is_last_stable_stable = m_ledger.check_stable(transaction_a, cache_a, last_stable_block_hash, best_pblock_hash, parents, block->from(), max_p_last_stable_block_hash, w_param);
			if (!is_last_stable_stable)
			{
				auto bp_block_state = cache_a->block_state_get(transaction_a, best_pblock_hash);
				result.code = mcp::validate_result_codes::invalid_block;
				result.err_msg = boost::str(boost::format("last stable block %1% is not stable in view of me, best parent: %2%, max parent last stable block: %3%, from: %4%, bp_block_state->earliest_bp_included_mc_index: %5%, bp_block_state->latest_bp_included_mc_index:%6%, bp_block_state->is_on_main_chain:%7%")
					% block->last_stable_block().to_string()
					% best_pblock_hash.to_string() % max_p_last_stable_block_hash.to_string() % block->from().hexPrefixed()
					% *bp_block_state->earliest_bp_included_mc_index % *bp_block_state->latest_bp_included_mc_index % bp_block_state->is_on_main_chain);
				return result;
			}
		}
	}

	{
		//mcp::stopwatch_guard sw("validate:last stable block stable2");
		if (best_pblock_hash != mcp::genesis::block_hash)
		{
			mcp::block_hash next_mc_hash;
			bool exists(!m_store.main_chain_get(transaction_a, last_stable_block_mci + 1, next_mc_hash));
			if (exists)
			{
				bool is_next_mc_block_stable(m_ledger.check_stable(transaction_a, cache_a, next_mc_hash, best_pblock_hash,
					parents, block->from(), last_stable_block_hash, w_param));
				if (is_next_mc_block_stable)
				{
					auto bp_block_state = cache_a->block_state_get(transaction_a, best_pblock_hash);
					result.code = mcp::validate_result_codes::invalid_block;
					result.err_msg = boost::str(boost::format("next mc block of last stable block %1% is stable in view of me, best parent: %2%, last stable block: %3%, from: %4%, bp_block_state->earliest_bp_included_mc_index: %5%, bp_block_state->latest_bp_included_mc_index:%6%, bp_block_state->is_on_main_chain:%7%")
						% next_mc_hash.to_string()
						% best_pblock_hash.to_string() % last_stable_block_hash.to_string() % block->from().hexPrefixed()
						% *bp_block_state->earliest_bp_included_mc_index % *bp_block_state->latest_bp_included_mc_index % bp_block_state->is_on_main_chain);
					return result;
				}
			}
		}
	}

	result.code = mcp::validate_result_codes::ok;
	return result;
}
//
//mcp::light_validate_result mcp::validation::light_validate(mcp::db::db_transaction & transaction_a, std::shared_ptr<mcp::iblock_cache> cache_a, std::shared_ptr<mcp::block> block)
//{
//	mcp::light_validate_result result;
//	mcp::block_type const & block_type(block->type());
//	assert_x(block_type == mcp::block_type::light);
//
//	mcp::block_hash const & block_hash(block->hash());
//	bool exists(false);
//	exists = cache_a->unlink_block_exists(transaction_a, block_hash);
//	if (!exists)
//		exists = cache_a->block_exists(transaction_a, block_hash);
//	if (exists)
//	{
//		result.code = mcp::light_validate_result_codes::old;
//		result.err_msg = boost::str(boost::format("block %1% exists") % block_hash.to_string());
//		return result;
//	}
//
//	mcp::block_hash const & previous(block->previous());
//	if (!previous.is_zero())
//	{
//		std::shared_ptr<mcp::block> previous_block = nullptr;
//		std::shared_ptr<mcp::unlink_block> previous_unlink_block = cache_a->unlink_block_get(transaction_a, previous);
//		if (nullptr == previous_unlink_block)
//		{
//			previous_block = cache_a->block_get(transaction_a, previous);
//			if (nullptr == previous_block)
//			{
//				result.code = mcp::light_validate_result_codes::missing_previous;
//				return result;
//			}
//		}
//		else
//			previous_block = previous_unlink_block->block;
//
//		//check previous from
//		if (previous_block->from() != block->from())
//		{
//			result.code = mcp::light_validate_result_codes::invalid_block;
//			result.err_msg = boost::str(boost::format("Invalid from %1%, not equal to previous from: %2%")
//				% block->from().to_account() % previous_block->from().to_account());
//			return result;
//		}
//	}
//
//	result.code = mcp::light_validate_result_codes::ok;
//	return result;
//}