--
-- PostgreSQL database dump
--

\restrict P8l4eju6Pbgd9WKJpcCAeqnFsN4VavesgSTPNaJBDLMGpbh77ar0epLDJBzz4qU

-- Dumped from database version 18.6
-- Dumped by pg_dump version 18.6

SET statement_timeout = 0;
SET lock_timeout = 0;
SET idle_in_transaction_session_timeout = 0;
SET transaction_timeout = 0;
SET client_encoding = 'UTF8';
SET standard_conforming_strings = on;
SELECT pg_catalog.set_config('search_path', '', false);
SET check_function_bodies = false;
SET xmloption = content;
SET client_min_messages = warning;
SET row_security = off;

SET default_tablespace = '';

SET default_table_access_method = heap;

--
-- Name: contracts; Type: TABLE; Schema: public; Owner: postgres
--

CREATE TABLE public.contracts (
    contract_transaction_unique_key text,
    contract_award_unique_key text,
    award_id_piid text,
    modification_number text,
    transaction_number bigint,
    parent_award_agency_id text,
    parent_award_agency_name text,
    parent_award_id_piid text,
    parent_award_modification_number text,
    federal_action_obligation double precision,
    total_dollars_obligated double precision,
    total_outlayed_amount_for_overall_award double precision,
    base_and_exercised_options_value double precision,
    current_total_value_of_award double precision,
    base_and_all_options_value double precision,
    potential_total_value_of_award double precision,
    disaster_emergency_fund_codes_for_overall_award text,
    outlayed_amount_from_covid_19_supplementals_for_overall_award double precision,
    obligated_amount_from_covid_19_supplementals_for_overall_award double precision,
    outlayed_amount_from_iija_supplemental_for_overall_award double precision,
    obligated_amount_from_iija_supplemental_for_overall_award double precision,
    action_date date,
    action_date_fiscal_year bigint,
    period_of_performance_start_date date,
    period_of_performance_current_end_date date,
    period_of_performance_potential_end_date timestamp without time zone,
    ordering_period_end_date date,
    solicitation_date date,
    awarding_agency_code text,
    awarding_agency_name text,
    awarding_sub_agency_code text,
    awarding_sub_agency_name text,
    awarding_office_code text,
    awarding_office_name text,
    funding_agency_code text,
    funding_agency_name text,
    funding_sub_agency_code text,
    funding_sub_agency_name text,
    funding_office_code text,
    funding_office_name text,
    treasury_accounts_funding_this_award text,
    federal_accounts_funding_this_award text,
    object_classes_funding_this_award text,
    program_activities_funding_this_award text,
    foreign_funding text,
    foreign_funding_description text,
    sam_exception bigint,
    sam_exception_description text,
    recipient_uei text,
    recipient_duns text,
    recipient_name text,
    recipient_name_raw text,
    recipient_doing_business_as_name text,
    cage_code text,
    recipient_parent_uei text,
    recipient_parent_duns text,
    recipient_parent_name text,
    recipient_parent_name_raw text,
    recipient_country_code text,
    recipient_country_name text,
    recipient_address_line_1 text,
    recipient_address_line_2 text,
    recipient_city_name text,
    prime_award_transaction_recipient_county_fips_code text,
    recipient_county_name text,
    prime_award_transaction_recipient_state_fips_code text,
    recipient_state_code text,
    recipient_state_name text,
    recipient_zip_4_code text,
    prime_award_transaction_recipient_cd_original text,
    prime_award_transaction_recipient_cd_current text,
    recipient_phone_number text,
    recipient_fax_number text,
    primary_place_of_performance_country_code text,
    primary_place_of_performance_country_name text,
    primary_place_of_performance_city_name text,
    prime_award_transaction_place_of_performance_county_fips_code text,
    primary_place_of_performance_county_name text,
    prime_award_transaction_place_of_performance_state_fips_code text,
    primary_place_of_performance_state_code text,
    primary_place_of_performance_state_name text,
    primary_place_of_performance_zip_4 text,
    prime_award_transaction_place_of_performance_cd_original text,
    prime_award_transaction_place_of_performance_cd_current text,
    award_or_idv_flag text,
    award_type_code text,
    award_type text,
    idv_type_code text,
    idv_type text,
    multiple_or_single_award_idv_code text,
    multiple_or_single_award_idv text,
    type_of_idc_code text,
    type_of_idc text,
    type_of_contract_pricing_code text,
    type_of_contract_pricing text,
    transaction_description text,
    prime_award_base_transaction_description text,
    action_type_code text,
    action_type text,
    solicitation_identifier text,
    number_of_actions bigint,
    inherently_governmental_functions text,
    inherently_governmental_functions_description text,
    product_or_service_code text,
    product_or_service_code_description text,
    contract_bundling_code text,
    contract_bundling text,
    dod_claimant_program_code text,
    dod_claimant_program_description text,
    naics_code bigint,
    naics_description text,
    recovered_materials_sustainability_code text,
    recovered_materials_sustainability text,
    domestic_or_foreign_entity_code text,
    domestic_or_foreign_entity text,
    dod_acquisition_program_code text,
    dod_acquisition_program_description text,
    information_technology_commercial_item_category_code text,
    information_technology_commercial_item_category text,
    epa_designated_product_code text,
    epa_designated_product text,
    country_of_product_or_service_origin_code text,
    country_of_product_or_service_origin text,
    place_of_manufacture_code text,
    place_of_manufacture text,
    subcontracting_plan_code text,
    subcontracting_plan text,
    extent_competed_code text,
    extent_competed text,
    solicitation_procedures_code text,
    solicitation_procedures text,
    type_of_set_aside_code text,
    type_of_set_aside text,
    evaluated_preference_code text,
    evaluated_preference text,
    research_code text,
    research text,
    fair_opportunity_limited_sources_code text,
    fair_opportunity_limited_sources text,
    other_than_full_and_open_competition_code text,
    other_than_full_and_open_competition text,
    number_of_offers_received bigint,
    commercial_item_acquisition_procedures_code text,
    commercial_item_acquisition_procedures text,
    small_business_competitiveness_demonstration_program boolean,
    simplified_procedures_for_certain_commercial_items_code text,
    simplified_procedures_for_certain_commercial_items boolean,
    a76_fair_act_action_code text,
    a76_fair_act_action boolean,
    fed_biz_opps_code text,
    fed_biz_opps text,
    local_area_set_aside_code boolean,
    local_area_set_aside text,
    price_evaluation_adjustment_preference_percent_difference double precision,
    clinger_cohen_act_planning_code boolean,
    clinger_cohen_act_planning text,
    materials_supplies_articles_equipment_code text,
    materials_supplies_articles_equipment text,
    labor_standards_code text,
    labor_standards text,
    construction_wage_rate_requirements_code text,
    construction_wage_rate_requirements text,
    interagency_contracting_authority_code text,
    interagency_contracting_authority text,
    other_statutory_authority text,
    program_acronym text,
    parent_award_type_code text,
    parent_award_type text,
    parent_award_single_or_multiple_code text,
    parent_award_single_or_multiple text,
    major_program text,
    national_interest_action_code text,
    national_interest_action text,
    cost_or_pricing_data_code text,
    cost_or_pricing_data text,
    cost_accounting_standards_clause_code text,
    cost_accounting_standards_clause text,
    government_furnished_property_code text,
    government_furnished_property text,
    sea_transportation_code text,
    sea_transportation text,
    undefinitized_action_code text,
    undefinitized_action text,
    consolidated_contract_code text,
    consolidated_contract text,
    performance_based_service_acquisition_code text,
    performance_based_service_acquisition text,
    multi_year_contract_code boolean,
    multi_year_contract text,
    contract_financing_code text,
    contract_financing text,
    purchase_card_as_payment_method_code text,
    purchase_card_as_payment_method boolean,
    contingency_humanitarian_or_peacekeeping_operation_code text,
    contingency_humanitarian_or_peacekeeping_operation text,
    alaskan_native_corporation_owned_firm boolean,
    american_indian_owned_business boolean,
    indian_tribe_federally_recognized boolean,
    native_hawaiian_organization_owned_firm boolean,
    tribally_owned_firm boolean,
    veteran_owned_business boolean,
    service_disabled_veteran_owned_business boolean,
    woman_owned_business boolean,
    women_owned_small_business boolean,
    economically_disadvantaged_women_owned_small_business boolean,
    joint_venture_women_owned_small_business boolean,
    joint_venture_economic_disadvantaged_women_owned_small_bus boolean,
    minority_owned_business boolean,
    subcontinent_asian_asian_indian_american_owned_business boolean,
    asian_pacific_american_owned_business boolean,
    black_american_owned_business boolean,
    hispanic_american_owned_business boolean,
    native_american_owned_business boolean,
    other_minority_owned_business boolean,
    contracting_officers_determination_of_business_size text,
    contracting_officers_determination_of_business_size_code text,
    emerging_small_business boolean,
    community_developed_corporation_owned_firm boolean,
    labor_surplus_area_firm boolean,
    us_federal_government boolean,
    federally_funded_research_and_development_corp boolean,
    federal_agency boolean,
    us_state_government boolean,
    us_local_government boolean,
    city_local_government boolean,
    county_local_government boolean,
    inter_municipal_local_government boolean,
    local_government_owned boolean,
    municipality_local_government boolean,
    school_district_local_government boolean,
    township_local_government boolean,
    us_tribal_government boolean,
    foreign_government boolean,
    organizational_type text,
    corporate_entity_not_tax_exempt boolean,
    corporate_entity_tax_exempt boolean,
    partnership_or_limited_liability_partnership boolean,
    sole_proprietorship boolean,
    small_agricultural_cooperative boolean,
    international_organization boolean,
    us_government_entity boolean,
    community_development_corporation boolean,
    domestic_shelter boolean,
    educational_institution boolean,
    foundation boolean,
    hospital_flag boolean,
    manufacturer_of_goods boolean,
    veterinary_hospital boolean,
    hispanic_servicing_institution boolean,
    receives_contracts boolean,
    receives_financial_assistance boolean,
    receives_contracts_and_financial_assistance boolean,
    airport_authority boolean,
    council_of_governments boolean,
    housing_authorities_public_tribal boolean,
    interstate_entity boolean,
    planning_commission boolean,
    port_authority boolean,
    transit_authority boolean,
    subchapter_scorporation boolean,
    limited_liability_corporation boolean,
    foreign_owned boolean,
    for_profit_organization boolean,
    nonprofit_organization boolean,
    other_not_for_profit_organization boolean,
    the_ability_one_program boolean,
    private_university_or_college boolean,
    state_controlled_institution_of_higher_learning boolean,
    c_1862_land_grant_college boolean,
    c_1890_land_grant_college boolean,
    c_1994_land_grant_college boolean,
    minority_institution boolean,
    historically_black_college boolean,
    tribal_college boolean,
    alaskan_native_servicing_institution boolean,
    native_hawaiian_servicing_institution boolean,
    school_of_forestry boolean,
    veterinary_college boolean,
    dot_certified_disadvantage boolean,
    self_certified_small_disadvantaged_business boolean,
    small_disadvantaged_business boolean,
    c8a_program_participant boolean,
    historically_underutilized_business_zone_hubzone_firm boolean,
    sba_certified_8a_joint_venture boolean,
    highly_compensated_officer_1_name text,
    highly_compensated_officer_1_amount double precision,
    highly_compensated_officer_2_name text,
    highly_compensated_officer_2_amount double precision,
    highly_compensated_officer_3_name text,
    highly_compensated_officer_3_amount double precision,
    highly_compensated_officer_4_name text,
    highly_compensated_officer_4_amount double precision,
    highly_compensated_officer_5_name text,
    highly_compensated_officer_5_amount double precision,
    usaspending_permalink text,
    initial_report_date timestamp without time zone,
    last_modified_date timestamp without time zone
);


ALTER TABLE public.contracts OWNER TO postgres;

--
-- Name: ix_contracts_action_date; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_action_date ON public.contracts USING btree (action_date);


--
-- Name: ix_contracts_action_date_fiscal_year; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_action_date_fiscal_year ON public.contracts USING btree (action_date_fiscal_year);


--
-- Name: ix_contracts_award_id_piid; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_award_id_piid ON public.contracts USING btree (award_id_piid);


--
-- Name: ix_contracts_awarding_agency_name; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_awarding_agency_name ON public.contracts USING btree (awarding_agency_name);


--
-- Name: ix_contracts_awarding_sub_agency_name; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_awarding_sub_agency_name ON public.contracts USING btree (awarding_sub_agency_name);


--
-- Name: ix_contracts_contract_award_unique_key; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_contract_award_unique_key ON public.contracts USING btree (contract_award_unique_key);


--
-- Name: ix_contracts_extent_competed; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_extent_competed ON public.contracts USING btree (extent_competed);


--
-- Name: ix_contracts_extent_competed_code; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_extent_competed_code ON public.contracts USING btree (extent_competed_code);


--
-- Name: ix_contracts_funding_agency_name; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_funding_agency_name ON public.contracts USING btree (funding_agency_name);


--
-- Name: ix_contracts_modification_number; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_modification_number ON public.contracts USING btree (modification_number);


--
-- Name: ix_contracts_naics_code; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_naics_code ON public.contracts USING btree (naics_code);


--
-- Name: ix_contracts_number_of_offers_received; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_number_of_offers_received ON public.contracts USING btree (number_of_offers_received);


--
-- Name: ix_contracts_parent_award_id_piid; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_parent_award_id_piid ON public.contracts USING btree (parent_award_id_piid);


--
-- Name: ix_contracts_parent_award_modification_number; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_parent_award_modification_number ON public.contracts USING btree (parent_award_modification_number);


--
-- Name: ix_contracts_product_or_service_code; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_product_or_service_code ON public.contracts USING btree (product_or_service_code);


--
-- Name: ix_contracts_product_or_service_code_description; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_product_or_service_code_description ON public.contracts USING btree (product_or_service_code_description);


--
-- Name: ix_contracts_recipient_name; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_recipient_name ON public.contracts USING btree (recipient_name);


--
-- Name: ix_contracts_recipient_name_raw; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_recipient_name_raw ON public.contracts USING btree (recipient_name_raw);


--
-- Name: ix_contracts_recipient_state_code; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_recipient_state_code ON public.contracts USING btree (recipient_state_code);


--
-- Name: ix_contracts_recipient_uei; Type: INDEX; Schema: public; Owner: postgres
--

CREATE INDEX ix_contracts_recipient_uei ON public.contracts USING btree (recipient_uei);


--
-- PostgreSQL database dump complete
--

\unrestrict P8l4eju6Pbgd9WKJpcCAeqnFsN4VavesgSTPNaJBDLMGpbh77ar0epLDJBzz4qU

